/**
 * Applies type analysis results to syntax and reports semantic errors.
 * Contextual rules may update expression types and emit diagnostics here.
 * Keep pure type computation and compatibility predicates in the other helpers.
 */

#include "semantic_analysis/type_analysis/type_rules.h"

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

}  // namespace sun::semantic_analysis::type_analysis
