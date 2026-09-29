/**
 * Applies type analysis results to syntax and reports semantic errors.
 * Contextual rules may update expression types and emit diagnostics here.
 * Keep pure type computation and compatibility predicates in the other helpers.
 */

#include "semantic_analysis/type_analysis/type_rules.h"

#include <set>

#include "semantic_analysis/expression_analysis/expression_properties.h"
#include "semantic_analysis/inference_result.h"
#include "semantic_analysis/type_analysis/type_inferer.h"
#include "support/error.h"

using sun::types::TypePtr;

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::ast::NumberExprAST;
using sun::parsing::TokenKind;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis::type_analysis {

using sun::types::unwrapRef;

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/**
 * True if `type` embeds enum `self` by value, walking enum payloads and class
 * fields. Pointers break the cycle (indirection is the fix we suggest).
 */
bool embedsEnumByValue(const TypePtr& type, const sun::types::EnumType* self,
                       std::set<const sun::types::Type*>& visited) {
  if (!type || !visited.insert(type.get()).second) return false;
  if (type->isEnum()) {
    auto* e = static_cast<const sun::types::EnumType*>(type.get());
    if (e->equals(*self)) return true;
    for (const auto& v : e->getVariants()) {
      for (const auto& pt : v.payloadTypes) {
        if (embedsEnumByValue(pt, self, visited)) return true;
      }
    }
  } else if (type->isClass()) {
    auto* c = static_cast<const sun::types::ClassType*>(type.get());
    for (const auto& field : c->getFields()) {
      if (embedsEnumByValue(field.type, self, visited)) return true;
    }
  }
  return false;
}

/**
 * True for the six comparison operators, whose result is a bool regardless of
 * what the operands are.
 */
bool isComparisonOp(TokenKind op) {
  return op == TokenKind::LESS || op == TokenKind::GREATER ||
         op == TokenKind::LESS_EQUAL || op == TokenKind::GREATER_EQUAL ||
         op == TokenKind::EQUAL_EQUAL || op == TokenKind::NOT_EQUAL;
}

}  // namespace

/** Coerces a fitting integer literal to the requested type without accepting
 * overflow. */
bool tryCoerceIntegerLiteral(ExprAST* expr, TypePtr targetType,
                             bool throwOnFail) {
  if (!expr || !targetType || !targetType->isPrimitive()) return false;
  if (expr->getType() != ASTNodeType::NUMBER) return false;

  const auto& numLit = static_cast<const NumberExprAST&>(*expr);
  if (!numLit.isInteger()) return false;

  // A suffixed literal (21u8) is typed: it never adapts to another type. Its
  // own suffix type still goes through the range check below.
  if (numLit.hasSuffix() &&
      !targetType->equals(*sun::types::Types::fromString(numLit.getSuffix()))) {
    return false;
  }

  if (literalFitsInType(numLit.getMagnitude(), numLit.isNegative(),
                        targetType->getKind())) {
    expr->setResolvedType(targetType);
    return true;
  }

  if (throwOnFail) {
    logAndThrowError("Integer literal " + numLit.getIntegerText() +
                         " cannot be represented as '" +
                         targetType->toDisplayString() + "'",
                     expr->getLocation());
  }
  return false;
}

/**
 * A char is a Unicode scalar value, not a small number: it compares with
 * another char and does nothing else. Both are an i32 underneath, so without
 * this check `'a' + 1` and `c == 65` would quietly take the integer path.
 */
void checkCharOperands(const sun::ast::BinaryExprAST& binExpr) {
  const ExprAST* lhs = binExpr.getLHS();
  const ExprAST* rhs = binExpr.getRHS();
  if (!lhs || !rhs) return;

  auto lhsType = unwrapRef(lhs->getResolvedType());
  auto rhsType = unwrapRef(rhs->getResolvedType());
  bool lhsIsChar = lhsType && lhsType->isChar();
  bool rhsIsChar = rhsType && rhsType->isChar();
  if (!lhsIsChar && !rhsIsChar) return;

  TokenKind op = binExpr.getOp().kind;
  if (!isComparisonOp(op)) {
    const auto& info = sun::parsing::getTokenInfo();
    auto it = info.find(op);
    std::string opText =
        it != info.end() ? std::string(it->second.text) : "operator";
    logAndThrowError(
        "'" + opText +
            "' is not defined for 'char'; convert it first with _convert<i32>",
        binExpr.getLocation());
  }
  if (!lhsIsChar || !rhsIsChar) {
    const TypePtr& other = lhsIsChar ? rhsType : lhsType;
    logAndThrowError(
        "Cannot compare 'char' with '" +
            (other ? other->toDisplayString() : std::string("unknown")) +
            "'; write the other side as a char literal, or convert the char "
            "with _convert<i32>",
        binExpr.getLocation());
  }
}

/**
 * An untyped numeric literal takes its type from context: the type the
 * surrounding expression expects, or failing that the operand it is combined
 * with. Without this the literal keeps its default i32/f64 type and codegen
 * widens the other operand to match, so `u8_var + 32` would produce an i32
 * value where semantic analysis promised a u8.
 */
void coerceBinaryLiteralOperands(const sun::ast::BinaryExprAST& binExpr,
                                 const TypePtr& expectedType) {
  // Returns true if the literal took the target type
  auto coerceNumericLiteral = [](const ExprAST* literal,
                                 const TypePtr& targetType) {
    auto target = unwrapRef(targetType);
    if (!literal || !target || !target->isPrimitive()) return false;
    auto* expr = const_cast<ExprAST*>(literal);

    const auto& num = static_cast<const NumberExprAST&>(*literal);
    if (num.isInteger()) {
      if (!target->isIntegral()) return false;
      return tryCoerceIntegerLiteral(expr, target, /*throwOnFail=*/false);
    }
    if (!target->isFloatingPoint()) return false;
    expr->setResolvedType(target);
    return true;
  };

  const ExprAST* lhs = binExpr.getLHS();
  const ExprAST* rhs = binExpr.getRHS();
  if (!lhs || !rhs) return;

  // A suffixed literal (21u8) is a typed operand, not a followable literal
  auto isUntypedLiteral = [](const ExprAST* e) {
    return e->getType() == ASTNodeType::NUMBER &&
           !static_cast<const NumberExprAST&>(*e).hasSuffix();
  };
  bool lhsIsLiteral = isUntypedLiteral(lhs);
  bool rhsIsLiteral = isUntypedLiteral(rhs);
  if (!lhsIsLiteral && !rhsIsLiteral) return;

  // A comparison's expected type describes its bool result, not its operands
  TokenKind op = binExpr.getOp().kind;
  auto expected = unwrapRef(expectedType);
  if (!isComparisonOp(op) && expected && expected->isNumeric()) {
    bool coerced = false;
    if (lhsIsLiteral) coerced = coerceNumericLiteral(lhs, expected) || coerced;
    if (rhsIsLiteral) coerced = coerceNumericLiteral(rhs, expected) || coerced;
    // The context type wins; adapting to the other operand would undo it
    if (coerced) return;
  }

  if (lhsIsLiteral && rhsIsLiteral) return;  // no typed operand to follow
  if (rhsIsLiteral) {
    coerceNumericLiteral(rhs, lhs->getResolvedType());
    return;
  }
  // A shift produces the left operand's type, so the shift amount must not
  // drag the result down to its own type.
  if (op == TokenKind::LEFT_SHIFT || op == TokenKind::RIGHT_SHIFT) return;
  coerceNumericLiteral(lhs, rhs->getResolvedType());
}

TypePtr unifyTernaryTypes(const TypePtr& thenType, const TypePtr& elseType,
                          std::optional<sun::support::Position> loc) {
  auto result = TypeInferer::branches(thenType, elseType,
                                      isAssignableTo(thenType, elseType),
                                      isAssignableTo(elseType, thenType));
  if (auto* type = std::get_if<TypePtr>(&result)) return *type;
  if (!thenType || !elseType)
    logAndThrowError("Cannot determine ternary branch types", loc);
  logAndThrowError("Ternary branch types do not match: '" +
                       thenType->toDisplayString() + "' vs '" +
                       elseType->toDisplayString() + "'",
                   loc);
}

/** Infers the element type from the first element, widened by the hint. */
void resolveArrayLiteralType(sun::ast::ArrayLiteralAST& arrLit,
                             const TypePtr& expectedType) {
  TypePtr first = arrLit.getElements().empty()
                      ? nullptr
                      : sun::semantic_analysis::requireResolvedType(
                            *arrLit.getElements().front());
  TypePtr expectedElement;
  if (auto hint = unwrapRef(expectedType); hint && hint->isArray())
    expectedElement =
        static_cast<const sun::types::ArrayType&>(*hint).getElementType();
  bool widen = first && expectedElement &&
               ((expectedElement->isInt64() && first->isInt32()) ||
                (expectedElement->isFloat64() && first->isFloat32()) ||
                expectedElement->equals(*first));
  arrLit.setResolvedType(requireInferredType(
      TypeInferer::array(first, arrLit.getElements().size(), expectedElement,
                         widen),
      arrLit.getLocation(),
      arrLit.getElements().empty() ? "Cannot infer type of empty array literal"
                                   : "Cannot infer array element type"));
}

/** Applies the payload allowlist, then rejects recursion by value. */
void validateEnumPayloadType(const TypePtr& type,
                             const sun::types::EnumType& enumType,
                             const std::string& variantName,
                             const sun::support::Position& location) {
  const std::string context = "Payload of variant '" + variantName +
                              "' in enum '" + enumType.getDisplayName() + "'";
  if (!type || type->isVoid()) {
    logAndThrowError(context + " cannot be void", location);
  }
  // Allowlist: primitives, pointers, enums, classes (including owning ones —
  // payload enums carry drop glue), interfaces (fat pointers are copyable
  // borrowed views), and references (the variant stores the referent's
  // address and owns nothing — this is what lets a container hand back
  // `Option<ref T>` for a peek instead of a copy of an element it still
  // owns). Arrays, slices, lambdas, threads etc. are deferred.
  bool allowed = type->isPrimitive() || type->isRawPointer() ||
                 type->isStaticPointer() || type->isEnum() || type->isClass() ||
                 type->isInterface() || type->isReference();
  if (!allowed) {
    logAndThrowError(context + " has unsupported type '" +
                         type->toDisplayString() +
                         "'; supported: primitives, pointers, enums, "
                         "interfaces, and classes",
                     location);
  }

  std::set<const sun::types::Type*> visited;
  if (embedsEnumByValue(type, &enumType, visited)) {
    logAndThrowError("Recursive enum '" + enumType.getDisplayName() +
                         "' requires indirection (raw_ptr)",
                     location);
  }
}

}  // namespace sun::semantic_analysis::type_analysis
