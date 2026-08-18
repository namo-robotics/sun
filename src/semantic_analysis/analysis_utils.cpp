// analysis_utils.cpp — Helper utilities for semantic analysis
//
// Contains type assignability checking, integer literal coercion,
// and type guard extraction.

#include "error.h"
#include "intrinsics.h"
#include "semantic_analyzer.h"

using sun::unwrapRef;

// Helper: check if an integer literal value fits in a target primitive type
static bool literalFitsInType(int64_t value,
                              const sun::PrimitiveType* primType) {
  switch (primType->getKind()) {
    case sun::Type::Kind::Int8:
      return value >= INT8_MIN && value <= INT8_MAX;
    case sun::Type::Kind::Int16:
      return value >= INT16_MIN && value <= INT16_MAX;
    case sun::Type::Kind::Int32:
      return value >= INT32_MIN && value <= INT32_MAX;
    case sun::Type::Kind::Int64:
      return true;  // int64_t always fits in i64
    case sun::Type::Kind::UInt8:
      return value >= 0 && value <= UINT8_MAX;
    case sun::Type::Kind::UInt16:
      return value >= 0 && value <= UINT16_MAX;
    case sun::Type::Kind::UInt32:
      return value >= 0 && static_cast<uint64_t>(value) <= UINT32_MAX;
    case sun::Type::Kind::UInt64:
      return value >= 0;  // int64_t can't represent full u64 range
    case sun::Type::Kind::Bool:
      return value == 0 || value == 1;
    default:
      return false;
  }
}

// Helper: try to coerce an integer literal to a target primitive type.
// Returns true if coercion happened (literal fits in target type).
// If throwOnFail is true, throws an error when the literal doesn't fit.
bool SemanticAnalyzer::tryCoerceIntegerLiteral(ExprAST* expr,
                                               sun::TypePtr targetType,
                                               bool throwOnFail) {
  if (!expr || !targetType || !targetType->isPrimitive()) return false;
  if (expr->getType() != ASTNodeType::NUMBER) return false;

  const auto& numLit = static_cast<const NumberExprAST&>(*expr);
  if (!numLit.isInteger()) return false;

  int64_t val = numLit.getIntVal();
  const auto* primType =
      static_cast<const sun::PrimitiveType*>(targetType.get());

  if (literalFitsInType(val, primType)) {
    expr->setResolvedType(targetType);
    return true;
  }

  if (throwOnFail) {
    logAndThrowError("Integer literal " + std::to_string(val) +
                         " cannot be represented as '" +
                         targetType->toString() + "'",
                     expr->getLocation());
  }
  return false;
}

// Bit width of an integer primitive, 0 for anything else.
static int integerBitWidth(const sun::TypePtr& type) {
  if (!type || !type->isPrimitive()) return 0;
  switch (type->getKind()) {
    case sun::Type::Kind::Int8:
    case sun::Type::Kind::UInt8:
      return 8;
    case sun::Type::Kind::Int16:
    case sun::Type::Kind::UInt16:
      return 16;
    case sun::Type::Kind::Int32:
    case sun::Type::Kind::UInt32:
      return 32;
    case sun::Type::Kind::Int64:
    case sun::Type::Kind::UInt64:
      return 64;
    default:
      return 0;
  }
}

// An untyped numeric literal takes its type from context: the type the
// surrounding expression expects, or failing that the operand it is combined
// with. Without this the literal keeps its default i32/f64 type and codegen
// widens the other operand to match, so `u8_var + 32` would produce an i32
// value where semantic analysis promised a u8.
void SemanticAnalyzer::coerceBinaryLiteralOperands(
    const BinaryExprAST& binExpr, const sun::TypePtr& expectedType) {
  // Returns true if the literal took the target type
  auto coerceNumericLiteral = [](const ExprAST* literal,
                                 const sun::TypePtr& targetType) {
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

  bool lhsIsLiteral = lhs->getType() == ASTNodeType::NUMBER;
  bool rhsIsLiteral = rhs->getType() == ASTNodeType::NUMBER;
  if (!lhsIsLiteral && !rhsIsLiteral) return;

  // A comparison's expected type describes its bool result, not its operands
  TokenKind op = binExpr.getOp().kind;
  bool isComparison = op == TokenKind::LESS || op == TokenKind::GREATER ||
                      op == TokenKind::LESS_EQUAL ||
                      op == TokenKind::GREATER_EQUAL ||
                      op == TokenKind::EQUAL_EQUAL || op == TokenKind::NOT_EQUAL;
  auto expected = unwrapRef(expectedType);
  if (!isComparison && expected && expected->isNumeric()) {
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

sun::TypePtr SemanticAnalyzer::promoteBinaryOperands(
    const sun::TypePtr& lhsType, const sun::TypePtr& rhsType) {
  auto lhs = unwrapRef(lhsType);
  auto rhs = unwrapRef(rhsType);
  if (!lhs || !rhs || lhs->getKind() == rhs->getKind()) return lhs;

  // Integers widen to the larger of the two; equal widths keep the LHS type
  int lhsBits = integerBitWidth(lhs);
  int rhsBits = integerBitWidth(rhs);
  if (lhsBits && rhsBits) return rhsBits > lhsBits ? rhs : lhs;

  // f32 widens to f64
  if (lhs->isFloatingPoint() && rhs->isFloatingPoint()) {
    return lhs->isFloat64() ? lhs : rhs;
  }
  return lhs;
}

sun::TypePtr SemanticAnalyzer::unifyTernaryTypes(const sun::TypePtr& thenType,
                                                 const sun::TypePtr& elseType,
                                                 std::optional<Position> loc) {
  if (!thenType || !elseType) {
    logAndThrowError("Cannot determine ternary branch types", loc);
  }
  if (thenType->equals(*elseType)) return thenType;

  bool thenToElse = isAssignableTo(thenType, elseType);
  bool elseToThen = isAssignableTo(elseType, thenType);
  if (thenToElse && elseToThen) {
    // Both directions hold for f32<->f64 and same-width integers; never
    // narrow to f32.
    if (thenType->getKind() == sun::Type::Kind::Float64) return thenType;
    if (elseType->getKind() == sun::Type::Kind::Float64) return elseType;
    return thenType;
  }
  if (thenToElse) return elseType;
  if (elseToThen) return thenType;

  logAndThrowError("Ternary branch types do not match: '" +
                       thenType->toString() + "' vs '" + elseType->toString() +
                       "'",
                   loc);
}

// Helper: extract type guard pattern from condition
// If condition is `_is<T>(var)`, returns (varName, narrowedType)
// Works for concrete types, interfaces, and type traits
std::optional<std::pair<std::string, sun::TypePtr>>
SemanticAnalyzer::extractTypeGuard(const ExprAST& cond) {
  // Must be a GenericCallAST with function name "_is"
  if (cond.getType() != ASTNodeType::GENERIC_CALL) return std::nullopt;

  const auto& genericCall = static_cast<const GenericCallAST&>(cond);
  if (sun::getIntrinsic(genericCall.getFunctionName()) != sun::Intrinsic::Is) {
    return std::nullopt;
  }

  // Must have exactly one argument that is a variable reference
  const auto& args = genericCall.getArgs();
  if (args.size() != 1) return std::nullopt;
  if (args[0]->getType() != ASTNodeType::VARIABLE_REFERENCE)
    return std::nullopt;

  const auto& varRef = static_cast<const VariableReferenceAST&>(*args[0]);
  const std::string& varName = varRef.getName();

  // Get the type argument
  const auto& typeArgs = genericCall.getTypeArguments();
  const std::string& typeName = typeArgs[0]->baseName;

  // Skip type traits (_Integer, _Float, etc.) - they don't narrow to a concrete
  // type
  if (sun::isTypeTrait(typeName)) {
    return std::nullopt;
  }

  // Check if it's an interface
  auto interfaceType = lookupInterface(typeName);
  if (interfaceType) {
    return std::make_pair(varName, interfaceType);
  }

  // Check if it's a class
  auto classType = lookupClass(typeName);
  if (classType) {
    return std::make_pair(varName, classType);
  }

  // Check if it's a primitive type
  sun::TypePtr primType = sun::Types::fromString(typeName);
  if (primType) {
    return std::make_pair(varName, primType);
  }

  return std::nullopt;
}

// -------------------------------------------------------------------
// Type assignability checking
// -------------------------------------------------------------------

// Check if a type can be assigned to another type.
// This implements the subtyping rules for Sun:
// - Exact type equality
// - Class C can be assigned to interface I if C implements I
// - ref T can be assigned to ref I if T is assignable to I
// - Numeric widening: smaller integers to larger (including signed/unsigned),
// f32 to f64
bool SemanticAnalyzer::isAssignableTo(const sun::TypePtr& from,
                                      const sun::TypePtr& to) {
  if (!from || !to) return false;

  // Exact equality always works
  if (from->equals(*to)) return true;

  // Numeric widening
  if (from->isPrimitive() && to->isPrimitive()) {
    auto fromKind = from->getKind();
    auto toKind = to->getKind();

    auto isInteger = [](sun::Type::Kind k) {
      return k == sun::Type::Kind::Int8 || k == sun::Type::Kind::Int16 ||
             k == sun::Type::Kind::Int32 || k == sun::Type::Kind::Int64 ||
             k == sun::Type::Kind::UInt8 || k == sun::Type::Kind::UInt16 ||
             k == sun::Type::Kind::UInt32 || k == sun::Type::Kind::UInt64;
    };

    auto intBitWidth = [](sun::Type::Kind k) -> int {
      switch (k) {
        case sun::Type::Kind::Int8:
        case sun::Type::Kind::UInt8:
          return 8;
        case sun::Type::Kind::Int16:
        case sun::Type::Kind::UInt16:
          return 16;
        case sun::Type::Kind::Int32:
        case sun::Type::Kind::UInt32:
          return 32;
        case sun::Type::Kind::Int64:
        case sun::Type::Kind::UInt64:
          return 64;
        default:
          return 0;
      }
    };

    // Allow integer widening (destination must be at least as wide)
    // This includes u8 -> i64, i32 -> i64, etc.
    if (isInteger(fromKind) && isInteger(toKind)) {
      return intBitWidth(fromKind) <= intBitWidth(toKind);
    }

    // Allow f32 <-> f64 conversions (both widening and narrowing)
    // This matches the existing permissive behavior for floating point
    if ((fromKind == sun::Type::Kind::Float32 ||
         fromKind == sun::Type::Kind::Float64) &&
        (toKind == sun::Type::Kind::Float32 ||
         toKind == sun::Type::Kind::Float64)) {
      return true;
    }
  }

  // Non-throwing lambda is accepted where a throwing lambda is expected
  if (to->isLambda() && from->isLambda()) {
    auto* toL = static_cast<const sun::LambdaType*>(to.get());
    auto* fromL = static_cast<const sun::LambdaType*>(from.get());
    return toL->canThrow() && !fromL->canThrow() &&
           fromL->equalsIgnoringThrow(*toL);
  }

  // Unwrap reference types and check inner compatibility
  if (to->isReference() && from->isReference()) {
    auto* toRef = static_cast<sun::ReferenceType*>(to.get());
    auto* fromRef = static_cast<sun::ReferenceType*>(from.get());
    return isAssignableTo(fromRef->getReferencedType(),
                          toRef->getReferencedType());
  }

  // Class-to-interface assignability:
  // Class C can be assigned to interface I if C implements I
  if (to->isInterface() && from->isClass()) {
    auto* ifaceType = static_cast<sun::InterfaceType*>(to.get());
    auto* classType = static_cast<sun::ClassType*>(from.get());
    return classType->convertibleToInterface(ifaceType->getName());
  }

  // Class -> ref Interface (class can be passed as ref to interface it
  // implements)
  if (to->isReference() && from->isClass()) {
    auto* toRef = static_cast<sun::ReferenceType*>(to.get());
    sun::TypePtr innerTo = toRef->getReferencedType();
    if (innerTo && innerTo->isInterface()) {
      auto* ifaceType = static_cast<sun::InterfaceType*>(innerTo.get());
      auto* classType = static_cast<sun::ClassType*>(from.get());
      return classType->convertibleToInterface(ifaceType->getName());
    }
  }

  // ref Class -> Interface (unwrap ref, check class implements interface)
  if (to->isInterface() && from->isReference()) {
    auto* fromRef = static_cast<sun::ReferenceType*>(from.get());
    sun::TypePtr innerFrom = fromRef->getReferencedType();
    if (innerFrom && innerFrom->isClass()) {
      auto* ifaceType = static_cast<sun::InterfaceType*>(to.get());
      auto* classType = static_cast<sun::ClassType*>(innerFrom.get());
      return classType->convertibleToInterface(ifaceType->getName());
    }
  }

  // ref(T) -> T: the value is read out of the reference. Fine for anything a
  // read can honestly duplicate, but not for a T that runs drop code: the
  // borrowed value and the copy would both own what it holds and both release
  // it. Moving out of a borrow is not a thing you can do — a container hands
  // ownership over with take()/pop()/remove().
  if (!to->isReference() && from->isReference()) {
    auto* fromRef = static_cast<sun::ReferenceType*>(from.get());
    if (sun::typeNeedsDrop(to)) return false;
    return isAssignableTo(fromRef->getReferencedType(), to);
  }

  // Class-to-class: compare by mangled name (unique identifier)
  // This handles cases where equals() fails due to different type instances
  if (to->isClass() && from->isClass()) {
    auto* toClass = static_cast<sun::ClassType*>(to.get());
    auto* fromClass = static_cast<sun::ClassType*>(from.get());
    return toClass->getMangledName() == fromClass->getMangledName();
  }

  return false;
}
