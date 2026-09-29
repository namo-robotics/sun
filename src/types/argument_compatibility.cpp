// argument_compatibility.cpp — Which values may stand in for which types

#include "types/argument_compatibility.h"

#include "types/type_utils.h"

/** Type definitions shared by analysis and code generation. */
namespace sun::types {

/** Reports whether a value of one type can be assigned to another. */
bool isAssignableTo(const TypePtr& from, const TypePtr& to) {
  if (!from || !to) return false;

  if (to->isInterface()) return false;

  // Exact equality always works
  if (from->equals(*to)) return true;

  // A non-throwing pointer may widen to the same throwing signature. The
  // reverse would let an indirect call bypass normal error handling.
  if (from->isFunction() && to->isFunction()) {
    const auto& source = static_cast<const FunctionType&>(*from);
    const auto& target = static_cast<const FunctionType&>(*to);
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
    auto* s = static_cast<const StaticPointerType*>(from.get());
    auto* r = static_cast<const RawPointerType*>(to.get());
    if (s->getPointeeType()->equals(*r->getPointeeType())) return true;
  }

  // Numeric widening
  if (from->isPrimitive() && to->isPrimitive()) {
    // Allow integer widening (destination must be at least as wide)
    // This includes u8 -> i64, i32 -> i64, etc.
    if (from->isIntegral() && to->isIntegral()) {
      return from->numericBitWidth() <= to->numericBitWidth();
    }

    // Allow f32 <-> f64 conversions (both widening and narrowing)
    // This matches the existing permissive behavior for floating point
    if (from->isFloatingPoint() && to->isFloatingPoint()) {
      return true;
    }
  }

  // Lambda widening: a non-throwing lambda is accepted where a throwing one
  // is expected, and an environment-free lambda where a '<'_>' one is
  // expected — never the reverse in either direction
  if (to->isLambda() && from->isLambda()) {
    auto* toL = static_cast<const LambdaType*>(to.get());
    auto* fromL = static_cast<const LambdaType*>(from.get());
    return toL->acceptsValueOf(*fromL);
  }

  // A borrow of a sized array stands in for a borrow of the unsized view
  // (the rank is erased); a sized array itself must match exactly.
  if (to->isArray() && from->isArray()) return false;

  if (ClassType::isInterfaceConvertible(from, to)) return true;

  // Unwrap reference types and check inner compatibility. A const borrow
  // never becomes a mutable one.
  if (to->isReference() && from->isReference()) {
    auto* toRef = static_cast<const ReferenceType*>(to.get());
    auto* fromRef = static_cast<const ReferenceType*>(from.get());
    if (!refMutabilityConvertible(*fromRef, *toRef)) return false;
    if (ClassType::isArrayCompatible(fromRef->getReferencedType(),
                                     toRef->getReferencedType())) {
      return true;
    }
    return isAssignableTo(fromRef->getReferencedType(),
                          toRef->getReferencedType());
  }

  // ref(T) -> T: the value is read out of the reference. Only a scalar can be
  // duplicated that way. A compound T read out of a borrow would be a second
  // value backed by the borrowed storage — borrow it with `ref`, copy it
  // explicitly with clone(), or move it out of a container with
  // pop()/remove()/swap_remove().
  if (!to->isReference() && from->isReference()) {
    auto* fromRef = static_cast<const ReferenceType*>(from.get());
    if (!typeCopiesByRead(to)) return false;
    return isAssignableTo(fromRef->getReferencedType(), to);
  }

  return false;
}

/** Checks the referent, borrow mutability, and unsized array view rules. */
bool referenceParameterAccepts(const ReferenceType& param,
                               const TypePtr& argType) {
  const TypePtr& referent = param.getReferencedType();
  if (referent->equals(*argType)) return true;
  if (argType->isReference()) {
    const auto& argRef = static_cast<const ReferenceType&>(*argType);
    if (refMutabilityConvertible(argRef, param) &&
        referent->equals(*argRef.getReferencedType()))
      return true;
  }
  if (referent->isArray() && argType->isArray()) {
    const auto& paramArray = static_cast<const ArrayType&>(*referent);
    const auto& argArray = static_cast<const ArrayType&>(*argType);
    if (paramArray.isUnsized() &&
        paramArray.getElementType()->equals(*argArray.getElementType()))
      return true;
  }
  return false;
}

/** Accepts any raw_ptr where a raw_ptr to i8 or u8 is expected. */
bool isBytePointerArgument(const TypePtr& argType, const TypePtr& paramType) {
  if (!argType->isRawPointer() || !paramType->isRawPointer()) return false;
  const auto& pointee =
      static_cast<const RawPointerType&>(*paramType).getPointeeType();
  return pointee->isInt8() || pointee->isUInt8();
}

/** Applies the call-site rules, then falls back to assignability. */
bool argumentAccepts(const TypePtr& argType, const TypePtr& paramType,
                     bool allowBytePointer) {
  if (paramType->equals(*argType)) return true;

  if (paramType->isReference()) {
    const auto& param = static_cast<const ReferenceType&>(*paramType);
    if (referenceParameterAccepts(param, argType)) return true;
    // Read through the pointer: raw_ptr<T> where ref T is expected
    if (argType->isRawPointer() &&
        static_cast<const RawPointerType&>(*argType).getPointeeType()->equals(
            *param.getReferencedType()))
      return true;
  }

  // Read through the pointer: raw_ptr<T> where a primitive T is expected
  if (argType->isRawPointer() && paramType->isPrimitive() &&
      static_cast<const RawPointerType&>(*argType).getPointeeType()->equals(
          *paramType))
    return true;

  // Null is compatible with any pointer type
  if (argType->isNullPointer() && paramType->isAnyPointer()) return true;

  if (allowBytePointer && isBytePointerArgument(argType, paramType))
    return true;

  return isAssignableTo(argType, paramType);
}

}  // namespace sun::types
