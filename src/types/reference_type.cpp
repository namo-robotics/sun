/** Implements borrow representation queries that require complete array types.
 */
#include "types/reference_type.h"

#include "types/array_type.h"

/** Implements shared type descriptions and queries. */
namespace sun::types {
bool ReferenceType::isUnsizedArrayRef() const {
  if (referencedType->isArray()) {
    return static_cast<const ArrayType*>(referencedType.get())->isUnsized();
  }
  return false;
}

// A reference is the referent's address, except a `ref array<T>` to an
// unsized array, which IS the view struct { ptr data, i32 ndims, ptr dims }:
// the view is born at the borrow site from a sized array and travels by value.
llvm::Type* ReferenceType::toLLVMType(llvm::LLVMContext& ctx) const {
  if (isUnsizedArrayRef()) return ArrayType::getArrayStructType(ctx);
  return llvm::PointerType::getUnqual(ctx);
}

}  // namespace sun::types
