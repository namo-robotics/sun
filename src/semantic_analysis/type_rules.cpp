// type_rules.cpp — Type rules that hold no analyzer state (see type_rules.h)

#include "semantic_analysis/type_rules.h"

#include "support/error.h"

using sun::semantic_analysis::ClassType;
using sun::semantic_analysis::ReferenceType;
using sun::semantic_analysis::TypePtr;

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::ast::NumberExprAST;
using sun::parsing::TokenKind;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

using sun::semantic_analysis::unwrapRef;

}  // namespace sun::semantic_analysis

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

bool literalFitsInType(uint64_t magnitude, bool negative,
                       sun::semantic_analysis::Type::Kind kind) {
  // A signed type of `bits` width holds -2^(bits-1) .. 2^(bits-1)-1; the
  // negative side reaches one further than the positive side.
  auto fitsSigned = [&](int bits) {
    const uint64_t half = uint64_t(1) << (bits - 1);
    return negative ? magnitude <= half : magnitude < half;
  };
  auto fitsUnsigned = [&](uint64_t max) {
    return !negative && magnitude <= max;
  };
  switch (kind) {
    case sun::semantic_analysis::Type::Kind::Int8:
      return fitsSigned(8);
    case sun::semantic_analysis::Type::Kind::Int16:
      return fitsSigned(16);
    case sun::semantic_analysis::Type::Kind::Int32:
      return fitsSigned(32);
    case sun::semantic_analysis::Type::Kind::Int64:
      return fitsSigned(64);
    case sun::semantic_analysis::Type::Kind::UInt8:
      return fitsUnsigned(UINT8_MAX);
    case sun::semantic_analysis::Type::Kind::UInt16:
      return fitsUnsigned(UINT16_MAX);
    case sun::semantic_analysis::Type::Kind::UInt32:
      return fitsUnsigned(UINT32_MAX);
    case sun::semantic_analysis::Type::Kind::UInt64:
      return fitsUnsigned(UINT64_MAX);
    case sun::semantic_analysis::Type::Kind::Bool:
      return fitsUnsigned(1);
    default:
      return false;
  }
}

/** Keeps the implementation helpers in this file private to this translation unit. */
namespace {

/**
 * Bit width of an integer primitive, 0 for anything else.
 */
int integerBitWidth(const TypePtr& type) {
  if (!type || !type->isPrimitive()) return 0;
  switch (type->getKind()) {
    case sun::semantic_analysis::Type::Kind::Int8:
    case sun::semantic_analysis::Type::Kind::UInt8:
      return 8;
    case sun::semantic_analysis::Type::Kind::Int16:
    case sun::semantic_analysis::Type::Kind::UInt16:
      return 16;
    case sun::semantic_analysis::Type::Kind::Int32:
    case sun::semantic_analysis::Type::Kind::UInt32:
      return 32;
    case sun::semantic_analysis::Type::Kind::Int64:
    case sun::semantic_analysis::Type::Kind::UInt64:
      return 64;
    default:
      return 0;
  }
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

bool tryCoerceIntegerLiteral(ExprAST* expr, TypePtr targetType,
                             bool throwOnFail) {
  if (!expr || !targetType || !targetType->isPrimitive()) return false;
  if (expr->getType() != ASTNodeType::NUMBER) return false;

  const auto& numLit = static_cast<const NumberExprAST&>(*expr);
  if (!numLit.isInteger()) return false;

  // A suffixed literal (21u8) is typed: it never adapts to another type. Its
  // own suffix type still goes through the range check below.
  if (numLit.hasSuffix() &&
      !targetType->equals(
          *sun::semantic_analysis::Types::fromString(numLit.getSuffix()))) {
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

TypePtr promoteBinaryOperands(const TypePtr& lhsType, const TypePtr& rhsType) {
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

TypePtr unifyTernaryTypes(const TypePtr& thenType, const TypePtr& elseType,
                          std::optional<sun::support::Position> loc) {
  if (!thenType || !elseType) {
    logAndThrowError("Cannot determine ternary branch types", loc);
  }
  if (thenType->equals(*elseType)) return thenType;

  bool thenToElse = isAssignableTo(thenType, elseType);
  bool elseToThen = isAssignableTo(elseType, thenType);
  if (thenToElse && elseToThen) {
    // Both directions hold for f32<->f64 and same-width integers; never
    // narrow to f32.
    if (thenType->getKind() == sun::semantic_analysis::Type::Kind::Float64)
      return thenType;
    if (elseType->getKind() == sun::semantic_analysis::Type::Kind::Float64)
      return elseType;
    return thenType;
  }
  if (thenToElse) return elseType;
  if (elseToThen) return thenType;

  logAndThrowError("Ternary branch types do not match: '" +
                       thenType->toDisplayString() + "' vs '" +
                       elseType->toDisplayString() + "'",
                   loc);
}

// -------------------------------------------------------------------
// Type assignability checking
// -------------------------------------------------------------------

bool isAssignableTo(const TypePtr& from, const TypePtr& to) {
  if (!from || !to) return false;

  // Exact equality always works
  if (from->equals(*to)) return true;

  // A static_ptr narrows to a raw_ptr of the same pointee (the data pointer
  // A non-throwing pointer may widen to the same throwing signature. The
  // reverse would let an indirect call bypass normal error handling.
  if (from->isFunction() && to->isFunction()) {
    const auto& source =
        static_cast<const sun::semantic_analysis::FunctionType&>(*from);
    const auto& target =
        static_cast<const sun::semantic_analysis::FunctionType&>(*to);
    if (source.canThrow() && !target.canThrow()) return false;
    if (source.requiresUnsafe() && !target.requiresUnsafe()) return false;
    if (!source.getReturnType()->equals(*target.getReturnType())) return false;
    if (source.getParamTypes().size() != target.getParamTypes().size())
      return false;
    for (size_t i = 0; i < source.getParamTypes().size(); ++i) {
      if (!source.getParamTypes()[i]->equals(*target.getParamTypes()[i]))
        return false;
    }
    return true;
  }
  // is extracted). The reverse never holds: a raw_ptr carries no length and
  // no promise the bytes are immortal, so it cannot become a static_ptr.
  if (from->isStaticPointer() && to->isRawPointer()) {
    auto* s = static_cast<const sun::semantic_analysis::StaticPointerType*>(
        from.get());
    auto* r =
        static_cast<const sun::semantic_analysis::RawPointerType*>(to.get());
    if (s->getPointeeType()->equals(*r->getPointeeType())) return true;
  }

  // Numeric widening
  if (from->isPrimitive() && to->isPrimitive()) {
    auto fromKind = from->getKind();
    auto toKind = to->getKind();

    auto isInteger = [](sun::semantic_analysis::Type::Kind k) {
      return k == sun::semantic_analysis::Type::Kind::Int8 ||
             k == sun::semantic_analysis::Type::Kind::Int16 ||
             k == sun::semantic_analysis::Type::Kind::Int32 ||
             k == sun::semantic_analysis::Type::Kind::Int64 ||
             k == sun::semantic_analysis::Type::Kind::UInt8 ||
             k == sun::semantic_analysis::Type::Kind::UInt16 ||
             k == sun::semantic_analysis::Type::Kind::UInt32 ||
             k == sun::semantic_analysis::Type::Kind::UInt64;
    };

    auto intBitWidth = [](sun::semantic_analysis::Type::Kind k) -> int {
      switch (k) {
        case sun::semantic_analysis::Type::Kind::Int8:
        case sun::semantic_analysis::Type::Kind::UInt8:
          return 8;
        case sun::semantic_analysis::Type::Kind::Int16:
        case sun::semantic_analysis::Type::Kind::UInt16:
          return 16;
        case sun::semantic_analysis::Type::Kind::Int32:
        case sun::semantic_analysis::Type::Kind::UInt32:
          return 32;
        case sun::semantic_analysis::Type::Kind::Int64:
        case sun::semantic_analysis::Type::Kind::UInt64:
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
    if ((fromKind == sun::semantic_analysis::Type::Kind::Float32 ||
         fromKind == sun::semantic_analysis::Type::Kind::Float64) &&
        (toKind == sun::semantic_analysis::Type::Kind::Float32 ||
         toKind == sun::semantic_analysis::Type::Kind::Float64)) {
      return true;
    }
  }

  // Lambda widening: a non-throwing lambda is accepted where a throwing one
  // is expected, and an environment-free lambda where a '<'_>' one is
  // expected — never the reverse in either direction
  if (to->isLambda() && from->isLambda()) {
    auto* toL =
        static_cast<const sun::semantic_analysis::LambdaType*>(to.get());
    auto* fromL =
        static_cast<const sun::semantic_analysis::LambdaType*>(from.get());
    return toL->acceptsValueOf(*fromL);
  }

  // A borrow of a sized array stands in for a borrow of the unsized view
  // (the rank is erased); a sized array itself must match exactly.
  if (to->isArray() && from->isArray()) return false;

  // Unwrap reference types and check inner compatibility. A const borrow
  // never becomes a mutable one.
  if (to->isReference() && from->isReference()) {
    auto* toRef = static_cast<const ReferenceType*>(to.get());
    auto* fromRef = static_cast<const ReferenceType*>(from.get());
    if (!sun::semantic_analysis::refMutabilityConvertible(*fromRef, *toRef))
      return false;
    if (ClassType::isArrayCompatible(fromRef->getReferencedType(),
                                     toRef->getReferencedType())) {
      return true;
    }
    return isAssignableTo(fromRef->getReferencedType(),
                          toRef->getReferencedType());
  }

  // Class-to-interface assignability:
  // Class C can be assigned to interface I if C implements I.
  // A frame-carrying class (one that can hold a '<'_>' lambda) never
  // converts by value: the interface type would erase the frame binding,
  // letting the value escape the frame its lambda environment lives in.
  if (to->isInterface() && from->isClass()) {
    if (sun::semantic_analysis::typeIsFrameCarrying(from)) return false;
    auto* ifaceType =
        static_cast<const sun::semantic_analysis::InterfaceType*>(to.get());
    auto* classType = static_cast<const ClassType*>(from.get());
    return classType->convertibleToInterface(*ifaceType);
  }

  // Class -> ref Interface (class can be passed as ref to interface it
  // implements)
  if (to->isReference() && from->isClass()) {
    auto* toRef = static_cast<const ReferenceType*>(to.get());
    TypePtr innerTo = toRef->getReferencedType();
    if (innerTo && innerTo->isInterface()) {
      auto* ifaceType =
          static_cast<const sun::semantic_analysis::InterfaceType*>(
              innerTo.get());
      auto* classType = static_cast<const ClassType*>(from.get());
      return classType->convertibleToInterface(*ifaceType);
    }
  }

  // ref Class -> Interface never converts: an interface value owns what it
  // points at, and a borrow cannot become an owner. A borrowed class reaches
  // an interface only through `ref Interface` (handled above).

  // ref(T) -> T: the value is read out of the reference. Only a scalar can be
  // duplicated that way. A compound T read out of a borrow would be a second
  // value backed by the borrowed storage — borrow it with `ref`, copy it
  // explicitly with clone(), or move it out of a container with
  // pop()/remove()/swap_remove().
  if (!to->isReference() && from->isReference()) {
    auto* fromRef = static_cast<const ReferenceType*>(from.get());
    if (!sun::semantic_analysis::typeCopiesByRead(to)) return false;
    return isAssignableTo(fromRef->getReferencedType(), to);
  }

  return false;
}

bool isBorrowableLvalue(const ExprAST& target) {
  ASTNodeType kind = target.getType();
  // A conditional picks one of two slots at runtime; it borrows if both
  // branches do.
  if (kind == ASTNodeType::TERNARY) {
    const auto& ternary = static_cast<const sun::ast::TernaryExprAST&>(target);
    return isBorrowableLvalue(*ternary.getThen()) &&
           isBorrowableLvalue(*ternary.getElse());
  }
  return kind == ASTNodeType::VARIABLE_REFERENCE ||
         kind == ASTNodeType::MEMBER_ACCESS || kind == ASTNodeType::INDEX;
}

bool alwaysExits(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::RETURN:
    case ASTNodeType::THROW:
      return true;
    case ASTNodeType::BLOCK: {
      // One exiting statement is enough: nothing after it runs
      const auto& block = static_cast<const sun::ast::BlockExprAST&>(expr);
      for (const auto& stmt : block.getBody()) {
        if (alwaysExits(*stmt)) return true;
      }
      return false;
    }
    case ASTNodeType::UNSAFE_BLOCK:
      return alwaysExits(
          static_cast<const sun::ast::UnsafeBlockAST&>(expr).getBody());
    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const sun::ast::IfExprAST&>(expr);
      return ifExpr.getElse() != nullptr && alwaysExits(*ifExpr.getThen()) &&
             alwaysExits(*ifExpr.getElse());
    }
    case ASTNodeType::MATCH: {
      // Every arm must exit, and no discriminant value may slip past the
      // arms: an enum match is checked for exhaustiveness elsewhere, and any
      // other match needs a wildcard to promise the same.
      const auto& matchExpr = static_cast<const sun::ast::MatchExprAST&>(expr);
      if (matchExpr.getArms().empty()) return false;
      bool sawWildcard = false;
      for (const auto& arm : matchExpr.getArms()) {
        if (!alwaysExits(*arm.body)) return false;
        if (arm.isWildcard) sawWildcard = true;
      }
      TypePtr discType = sun::semantic_analysis::unwrapRef(
          matchExpr.getDiscriminant()->getResolvedType());
      return sawWildcard || (discType && discType->isEnum());
    }
    case ASTNodeType::TRY_CATCH: {
      // The body may stop part-way and land in a catch, so every clause has
      // to exit as well
      const auto& tryCatch =
          static_cast<const sun::ast::TryCatchExprAST&>(expr);
      if (!alwaysExits(tryCatch.getTryBlock())) return false;
      for (const auto& clause : tryCatch.getCatchClauses()) {
        if (!clause.body || !alwaysExits(*clause.body)) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

}  // namespace sun::semantic_analysis
