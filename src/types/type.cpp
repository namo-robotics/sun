/** Implements base-type queries that inspect concrete type representations. */
#include "types/type.h"

#include "types/enum_type.h"
#include "types/pointer_types.h"

/** Implements shared type descriptions and queries. */
namespace sun::types {
bool Type::isCompound() const {
  if (isEnum()) {
    return static_cast<const EnumType*>(this)->hasPayload();
  }
  return !isPrimitive() && !isReference() && !isRawPointer() &&
         !isStaticPointer() && !isFunction() && !isLambda() &&
         !isTypeParameter();
}

bool Type::isNumeric() const {
  Kind k = getKind();
  return k == Kind::Int8 || k == Kind::Int16 || k == Kind::Int32 ||
         k == Kind::Int64 || k == Kind::UInt8 || k == Kind::UInt16 ||
         k == Kind::UInt32 || k == Kind::UInt64 || k == Kind::Float32 ||
         k == Kind::Float64;
}

bool Type::isIntegral() const {
  Kind k = getKind();
  return k == Kind::Int8 || k == Kind::Int16 || k == Kind::Int32 ||
         k == Kind::Int64 || k == Kind::UInt8 || k == Kind::UInt16 ||
         k == Kind::UInt32 || k == Kind::UInt64;
}

bool Type::isFloatingPoint() const {
  Kind k = getKind();
  return k == Kind::Float32 || k == Kind::Float64;
}

bool Type::isString() const {
  // String is now represented as static_ptr<u8> - immortal string literal data
  if (auto* p = dynamic_cast<const StaticPointerType*>(this)) {
    return p->getPointeeType()->isUInt8();
  }
  return false;
}

}  // namespace sun::types
