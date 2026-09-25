/** Queries reference, ownership, lifetime, and diagnostic properties of types.
 */
#pragma once

#include "types/reference_type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * True if a read can honestly duplicate a value of this type. Scalars can:
 * primitives, pointers, functions. A class, payload enum, interface or array
 * value cannot: it has one owner, so reading one out of a borrow would hand
 * back a second value backed by the borrowed storage. Borrow it with `ref`
 * instead, or copy it explicitly with a clone method. Unbound type parameters
 * answer true; the specialization is checked with the concrete type in hand.
 */
inline bool typeCopiesByRead(const Type* type) {
  return type && !type->isCompound();
}

/**
 * Reports whether reading this type can duplicate its value without
 * transferring ownership.
 */
inline bool typeCopiesByRead(const TypePtr& type) {
  return typeCopiesByRead(type.get());
}

/**
 * True if reading a value of this type out of a place MOVES it: an owned
 * compound value. A borrow stays put and a scalar copies.
 */
inline bool typeMovesOnRead(const Type* type) {
  return type && !type->isReference() && !typeCopiesByRead(type);
}

/** Reports whether reading this type transfers ownership of its value. */
inline bool typeMovesOnRead(const TypePtr& type) {
  return typeMovesOnRead(type.get());
}

/**
 * "i32, ref Vec<i32>" — a list of types as it reads in a diagnostic.
 */
inline std::string formatTypeList(const std::vector<TypePtr>& types) {
  std::string out;
  for (size_t i = 0; i < types.size(); ++i) {
    if (i > 0) out += ", ";
    out += types[i] ? types[i]->toDisplayString() : "unknown";
  }
  return out;
}

/**
 * Helper: unwrap reference types (ref(T) -> T, otherwise unchanged)
 * References should behave like values, transparently dereferenced
 */
inline TypePtr unwrapRef(TypePtr type) {
  if (type && type->isReference()) {
    return static_cast<const ReferenceType*>(type.get())->getReferencedType();
  }
  return type;
}

/**
 * Reports whether both types name different interfaces after unwrapping
 * references. This does not check whether either interface extends the other.
 */
inline bool areDifferentInterfaces(const TypePtr& sourceType,
                                   const TypePtr& targetType) {
  auto source = unwrapRef(sourceType);
  auto target = unwrapRef(targetType);
  return source && source->isInterface() && target && target->isInterface() &&
         !source->equals(*target);
}

/**
 * `ref T` (the referent may be changed through it)
 */
inline bool isMutableRef(const TypePtr& type) {
  return type && type->isReference() &&
         static_cast<const ReferenceType*>(type.get())->isMutable();
}

/**
 * `const ref T` (the referent may only be read through it)
 */
inline bool isConstRef(const TypePtr& type) {
  return type && type->isReference() &&
         !static_cast<const ReferenceType*>(type.get())->isMutable();
}

/**
 * A reference of kind `from` may stand in for one of kind `to` unless that
 * would let a const borrow be written through
 */
inline bool refMutabilityConvertible(const ReferenceType& from,
                                     const ReferenceType& to) {
  return from.isMutable() || !to.isMutable();
}

/** Reports whether values of this type require cleanup when their lifetime
 * ends. */
bool typeNeedsDrop(const Type* type);
/** Reports whether values of this type require cleanup when their lifetime
 * ends. */
bool typeNeedsDrop(const TypePtr& type);
/** Reports whether this type can retain storage tied to a stack frame. */
bool typeIsFrameCarrying(const Type* type);
/** Reports whether this type can retain storage tied to a stack frame. */
bool typeIsFrameCarrying(const TypePtr& type);
/** Copies a type with relative lifetime names removed, preserving lifetime
 * markers. */
TypePtr eraseLifetimeNames(const TypePtr& type);

}  // namespace sun::types
