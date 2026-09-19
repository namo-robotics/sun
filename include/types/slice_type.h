/** Describes the builtin indexing range type. */
#pragma once

#include "llvm/IR/DerivedTypes.h"
#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * Slice type: builtin struct for array/matrix indexing
 * Represents a range [start, end) or a single index (when end = start + 1)
 * Used with IIndexable interface for uniform slice handling
 * LLVM representation: { i64 start, i64 end }
 */
class SliceType : public Type {
 public:
  /** Creates an instance with its default state. */
  SliceType() = default;

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Slice;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "slice"; }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override { return other.isSlice(); }

  /**
   * Get the LLVM struct type for slice: { i64, i64 }
   */
  static llvm::StructType* getSliceStructType(llvm::LLVMContext& ctx) {
    return llvm::StructType::get(ctx,
                                 {
                                     llvm::Type::getInt64Ty(ctx),  // start
                                     llvm::Type::getInt64Ty(ctx)   // end
                                 });
  }

  /** Returns the LLVM type used to represent values of this semantic type. */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return getSliceStructType(ctx);
  }

  /**
   * Helper to check if a slice is a single index (end == start + 1)
   * This is used to distinguish m[5] from m[5:6]
   */
  static bool isSingleIndex(int64_t start, int64_t end) {
    return end == start + 1;
  }
};

}  // namespace sun::types
