/**
 * Checks compatibility using supplied, read-only type descriptions.
 * Keep these predicates independent of semantic sessions, expression ASTs,
 * and diagnostics. Contextual coercion and error reporting belong in
 * type_rules.
 */
#include "semantic_analysis/type_analysis/type_checking.h"

/** Pure type inference and compatibility rules used by semantic analysis. */
namespace sun::semantic_analysis::type_analysis {
using sun::types::ClassType;
using sun::types::ReferenceType;
using sun::types::Type;
using sun::types::TypePtr;

/** Reports whether an integer magnitude and sign fit the target numeric type.
 */
bool literalFitsInType(uint64_t magnitude, bool negative,
                       sun::types::Type::Kind kind) {
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
    case sun::types::Type::Kind::Int8:
      return fitsSigned(8);
    case sun::types::Type::Kind::Int16:
      return fitsSigned(16);
    case sun::types::Type::Kind::Int32:
      return fitsSigned(32);
    case sun::types::Type::Kind::Int64:
      return fitsSigned(64);
    case sun::types::Type::Kind::UInt8:
      return fitsUnsigned(UINT8_MAX);
    case sun::types::Type::Kind::UInt16:
      return fitsUnsigned(UINT16_MAX);
    case sun::types::Type::Kind::UInt32:
      return fitsUnsigned(UINT32_MAX);
    case sun::types::Type::Kind::UInt64:
      return fitsUnsigned(UINT64_MAX);
    case sun::types::Type::Kind::Bool:
      return fitsUnsigned(1);
    default:
      return false;
  }
}

// -------------------------------------------------------------------
// Type assignability checking
// -------------------------------------------------------------------

/** Reports whether a value of one type can be assigned to another. */
bool isAssignableTo(const TypePtr& from, const TypePtr& to) {
  if (!from || !to) return false;

  // Exact equality always works
  if (from->equals(*to)) return true;

  // A non-throwing pointer may widen to the same throwing signature. The
  // reverse would let an indirect call bypass normal error handling.
  if (from->isFunction() && to->isFunction()) {
    const auto& source = static_cast<const sun::types::FunctionType&>(*from);
    const auto& target = static_cast<const sun::types::FunctionType&>(*to);
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
  // A static_ptr narrows to a raw_ptr by extracting its data pointer.
  // The reverse never holds: a raw_ptr carries no length and
  // no promise the bytes are immortal, so it cannot become a static_ptr.
  if (from->isStaticPointer() && to->isRawPointer()) {
    auto* s = static_cast<const sun::types::StaticPointerType*>(from.get());
    auto* r = static_cast<const sun::types::RawPointerType*>(to.get());
    if (s->getPointeeType()->equals(*r->getPointeeType())) return true;
  }

  // Numeric widening
  if (from->isPrimitive() && to->isPrimitive()) {
    auto fromKind = from->getKind();
    auto toKind = to->getKind();

    auto isInteger = [](sun::types::Type::Kind k) {
      return k == sun::types::Type::Kind::Int8 ||
             k == sun::types::Type::Kind::Int16 ||
             k == sun::types::Type::Kind::Int32 ||
             k == sun::types::Type::Kind::Int64 ||
             k == sun::types::Type::Kind::UInt8 ||
             k == sun::types::Type::Kind::UInt16 ||
             k == sun::types::Type::Kind::UInt32 ||
             k == sun::types::Type::Kind::UInt64;
    };

    auto intBitWidth = [](sun::types::Type::Kind k) -> int {
      switch (k) {
        case sun::types::Type::Kind::Int8:
        case sun::types::Type::Kind::UInt8:
          return 8;
        case sun::types::Type::Kind::Int16:
        case sun::types::Type::Kind::UInt16:
          return 16;
        case sun::types::Type::Kind::Int32:
        case sun::types::Type::Kind::UInt32:
          return 32;
        case sun::types::Type::Kind::Int64:
        case sun::types::Type::Kind::UInt64:
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
    if ((fromKind == sun::types::Type::Kind::Float32 ||
         fromKind == sun::types::Type::Kind::Float64) &&
        (toKind == sun::types::Type::Kind::Float32 ||
         toKind == sun::types::Type::Kind::Float64)) {
      return true;
    }
  }

  // Lambda widening: a non-throwing lambda is accepted where a throwing one
  // is expected, and an environment-free lambda where a '<'_>' one is
  // expected — never the reverse in either direction
  if (to->isLambda() && from->isLambda()) {
    auto* toL = static_cast<const sun::types::LambdaType*>(to.get());
    auto* fromL = static_cast<const sun::types::LambdaType*>(from.get());
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
    if (!sun::types::refMutabilityConvertible(*fromRef, *toRef)) return false;
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
    if (sun::types::typeIsFrameCarrying(from)) return false;
    auto* ifaceType = static_cast<const sun::types::InterfaceType*>(to.get());
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
          static_cast<const sun::types::InterfaceType*>(innerTo.get());
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
    if (!sun::types::typeCopiesByRead(to)) return false;
    return isAssignableTo(fromRef->getReferencedType(), to);
  }

  return false;
}

}  // namespace sun::semantic_analysis::type_analysis
