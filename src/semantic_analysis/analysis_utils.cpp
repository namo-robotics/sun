// analysis_utils.cpp — Helper utilities for semantic analysis
//
// Contains type assignability checking, integer literal coercion,
// and type guard extraction.

#include "support/error.h"
#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/semantic_analyzer.h"

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
                         targetType->toDisplayString() + "'",
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

// A char is a Unicode scalar value, not a small number: it compares with
// another char and does nothing else. Both are an i32 underneath, so without
// this check `'a' + 1` and `c == 65` would quietly take the integer path.
void SemanticAnalyzer::checkCharOperands(const BinaryExprAST& binExpr) {
  const ExprAST* lhs = binExpr.getLHS();
  const ExprAST* rhs = binExpr.getRHS();
  if (!lhs || !rhs) return;

  auto lhsType = unwrapRef(lhs->getResolvedType());
  auto rhsType = unwrapRef(rhs->getResolvedType());
  bool lhsIsChar = lhsType && lhsType->isChar();
  bool rhsIsChar = rhsType && rhsType->isChar();
  if (!lhsIsChar && !rhsIsChar) return;

  TokenKind op = binExpr.getOp().kind;
  bool isComparison = op == TokenKind::LESS || op == TokenKind::GREATER ||
                      op == TokenKind::LESS_EQUAL ||
                      op == TokenKind::GREATER_EQUAL ||
                      op == TokenKind::EQUAL_EQUAL || op == TokenKind::NOT_EQUAL;
  if (!isComparison) {
    const auto& info = getTokenInfo();
    auto it = info.find(op);
    std::string opText =
        it != info.end() ? std::string(it->second.text) : "operator";
    logAndThrowError(
        "'" + opText +
            "' is not defined for 'char'; convert it first with _convert<i32>",
        binExpr.getLocation());
  }
  if (!lhsIsChar || !rhsIsChar) {
    const sun::TypePtr& other = lhsIsChar ? rhsType : lhsType;
    logAndThrowError(
        "Cannot compare 'char' with '" +
            (other ? other->toDisplayString() : std::string("unknown")) +
            "'; write the other side as a char literal, or convert the char "
            "with _convert<i32>",
        binExpr.getLocation());
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
                       thenType->toDisplayString() + "' vs '" +
                       elseType->toDisplayString() + "'",
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

  // A static_ptr narrows to a raw_ptr of the same pointee (the data pointer
  // is extracted). The reverse never holds: a raw_ptr carries no length and
  // no promise the bytes are immortal, so it cannot become a static_ptr.
  if (from->isStaticPointer() && to->isRawPointer()) {
    auto* s = static_cast<const sun::StaticPointerType*>(from.get());
    auto* r = static_cast<const sun::RawPointerType*>(to.get());
    if (s->getPointeeType()->equals(*r->getPointeeType())) return true;
  }

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

  // Unwrap reference types and check inner compatibility. A const borrow
  // never becomes a mutable one.
  if (to->isReference() && from->isReference()) {
    auto* toRef = static_cast<sun::ReferenceType*>(to.get());
    auto* fromRef = static_cast<sun::ReferenceType*>(from.get());
    if (!sun::refMutabilityConvertible(*fromRef, *toRef)) return false;
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

  // ref(T) -> T: the value is read out of the reference. Only a scalar can be
  // duplicated that way. A compound T read out of a borrow would be a second
  // value backed by the borrowed storage — borrow it with `ref`, copy it
  // explicitly with clone(), or move it out of a container with
  // take()/pop()/remove().
  if (!to->isReference() && from->isReference()) {
    auto* fromRef = static_cast<sun::ReferenceType*>(from.get());
    if (!sun::typeCopiesByRead(to)) return false;
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

// -------------------------------------------------------------------
// Constness
// -------------------------------------------------------------------

std::string SemanticAnalyzer::immutableBaseOf(const ExprAST& place) {
  switch (place.getType()) {
    case ASTNodeType::PAREN_EXPR:
      return immutableBaseOf(
          *static_cast<const ParenExprAST&>(place).getInner());

    case ASTNodeType::MEMBER_ACCESS: {
      const auto& access = static_cast<const MemberAccessAST&>(place);
      sun::TypePtr objectType = access.getObject()->getResolvedType();
      // mod.name names the module's own variable, so its own constness
      // decides — a module has no mutability of its own to inherit
      if (objectType && objectType->isModule()) {
        const auto& mod = static_cast<const sun::ModuleType&>(*objectType);
        SymbolMatch match =
            findSymbolInModule(mod.getModulePath(), access.getMemberName());
        if (match.kind != SymbolKind::Variable || !match.variableInfo) {
          return "";
        }
        // display() names the declaring module without any library-hash scope
        std::string full = match.variableInfo->qualifiedName.display();
        if (match.variableInfo->isConst) return "constant '" + full + "'";
        if (sun::isConstRef(match.variableInfo->type)) {
          return "const reference '" + full + "'";
        }
        return "";
      }
      // Through a mutable borrow the referent may be changed
      if (sun::isMutableRef(objectType)) return "";
      return immutableBaseOf(*access.getObject());
    }

    case ASTNodeType::INDEX: {
      const auto& index = static_cast<const IndexAST&>(place);
      if (sun::isMutableRef(index.getTarget()->getResolvedType())) return "";
      return immutableBaseOf(*index.getTarget());
    }

    case ASTNodeType::TERNARY: {
      const auto& ternary = static_cast<const TernaryExprAST&>(place);
      std::string why = immutableBaseOf(*ternary.getThen());
      return why.empty() ? immutableBaseOf(*ternary.getElse()) : why;
    }

    case ASTNodeType::THIS: {
      VariableInfo* info = lookupVariable("this");
      if (info && info->isConst) return "'this' inside a const method";
      return "";
    }

    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& ref = static_cast<const VariableReferenceAST&>(place);
      VariableInfo* info = lookupVariable(ref.getName());
      if (!info) return "";
      if (info->isConst) return "constant '" + ref.getName() + "'";
      if (sun::isConstRef(info->type))
        return "const reference '" + ref.getName() + "'";
      return "";
    }

    default:
      // A call result or other temporary: only a const borrow is frozen
      if (sun::isConstRef(place.getResolvedType())) {
        return "a const reference";
      }
      return "";
  }
}

void SemanticAnalyzer::requireMutablePlace(const ExprAST& place,
                                           const std::string& action,
                                           const Position& loc) {
  std::string why = immutableBaseOf(place);
  if (!why.empty()) {
    logAndThrowError("Cannot " + action + " " + why, loc);
  }
}

void SemanticAnalyzer::checkMoveSource(const ExprAST& value,
                                       const Position& loc) {
  const ExprAST* source = &value;
  while (source->getType() == ASTNodeType::PAREN_EXPR) {
    source = static_cast<const ParenExprAST*>(source)->getInner();
  }
  sun::TypePtr type = source->getResolvedType();
  // Only an owned compound value moves; scalars and arrays (a fat pointer
  // into storage owned elsewhere) copy, and borrows stay put
  if (!type || type->isReference() || sun::typeCopiesByRead(type)) return;

  if (source->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& access = static_cast<const MemberAccessAST&>(*source);
    std::string why = immutableBaseOf(*access.getObject());
    if (!why.empty()) {
      logAndThrowError("Cannot move field '" + access.getMemberName() +
                           "' out of " + why +
                           "; borrow it with 'const ref' or copy it with "
                           "clone()",
                       loc);
    }
  } else if (source->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& ref = static_cast<const VariableReferenceAST&>(*source);
    VariableInfo* info = lookupVariable(ref.getName());
    if (info && info->isConst && info->isGlobal) {
      logAndThrowError("Cannot move constant global '" + ref.getName() +
                           "'; borrow it with 'const ref' or copy it with "
                           "clone()",
                       loc);
    }
  }
}

void SemanticAnalyzer::checkArgumentPlaces(
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::TypePtr>& paramTypes, const std::string& callee,
    const Position& loc) {
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!args[i] || !paramTypes[i]) continue;
    sun::TypePtr argType = args[i]->getResolvedType();
    if (sun::isMutableRef(paramTypes[i])) {
      // A reference argument is checked by assignability (const ref never
      // becomes ref); a place argument is borrowed here
      if (argType && argType->isReference()) continue;
      requireMutablePlace(*args[i],
                          "pass as 'ref' argument " + std::to_string(i + 1) +
                              " of '" + callee + "'",
                          loc);
    } else if (!paramTypes[i]->isReference()) {
      checkMoveSource(*args[i], loc);
    }
  }
}

bool SemanticAnalyzer::checkMethodReceiver(const ExprAST& receiver,
                                           const std::string& name,
                                           bool methodIsConst,
                                           bool isConstructor,
                                           const Position& loc) {
  std::string why = immutableBaseOf(receiver);
  if (why.empty()) return false;
  if (!methodIsConst && !isConstructor) {
    logAndThrowError("Cannot call non-const method '" + name + "' on " + why +
                         "; declare it 'const function' if it does not "
                         "change the object",
                     loc);
  }
  return true;
}
