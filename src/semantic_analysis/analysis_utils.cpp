// analysis_utils.cpp — Helper utilities for semantic analysis
//
// Contains type assignability checking, integer literal coercion,
// and type guard extraction.

#include "error.h"
#include "intrinsics.h"
#include "semantic_analyzer.h"

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
    return classType->implementsInterface(ifaceType->getName());
  }

  // Class -> ref Interface (class can be passed as ref to interface it
  // implements)
  if (to->isReference() && from->isClass()) {
    auto* toRef = static_cast<sun::ReferenceType*>(to.get());
    sun::TypePtr innerTo = toRef->getReferencedType();
    if (innerTo && innerTo->isInterface()) {
      auto* ifaceType = static_cast<sun::InterfaceType*>(innerTo.get());
      auto* classType = static_cast<sun::ClassType*>(from.get());
      return classType->implementsInterface(ifaceType->getName());
    }
  }

  // ref Class -> Interface (unwrap ref, check class implements interface)
  if (to->isInterface() && from->isReference()) {
    auto* fromRef = static_cast<sun::ReferenceType*>(from.get());
    sun::TypePtr innerFrom = fromRef->getReferencedType();
    if (innerFrom && innerFrom->isClass()) {
      auto* ifaceType = static_cast<sun::InterfaceType*>(to.get());
      auto* classType = static_cast<sun::ClassType*>(innerFrom.get());
      return classType->implementsInterface(ifaceType->getName());
    }
  }

  // ref(T) -> T: value can be extracted from reference (copy semantics)
  if (!to->isReference() && from->isReference()) {
    auto* fromRef = static_cast<sun::ReferenceType*>(from.get());
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
