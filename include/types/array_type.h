/** Describes array elements, dimensions, and LLVM views. */
#pragma once

#include <numeric>
#include <vector>

#include "llvm/IR/DerivedTypes.h"
#include "semantic_analysis/struct_names.h"
#include "support/error.h"
#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
using sun::support::logAndThrowError;

/**
 * Array type: array<T, N> or array<T, M, N> for multi-dimensional.
 * A sized array OWNS its elements inline - [N x T] wherever it lives (a local,
 * a field, a global, an element of another array) - and moves like every
 * other compound value. An unsized array&lt;T&gt; (empty dimensions) is a view
 * of some sized array with the rank erased; it exists only behind `ref` and is
 * carried as the fat struct { ptr data, i32 ndims, ptr dims }.
 */
class ArrayType : public Type {
  TypePtr elementType;             // The element type (e.g., i32)
  std::vector<size_t> dimensions;  // Fixed sizes (e.g., {5} or {3, 2}), empty
                                   // means unsized

 public:
  /** Creates a fixed-size array type with the supplied element type and
   * dimensions. */
  ArrayType(TypePtr elemType, std::vector<size_t> dims)
      : elementType(std::move(elemType)), dimensions(std::move(dims)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Array;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the element type stored by this object. */
  const TypePtr& getElementType() const { return elementType; }
  /** Returns the array dimension sizes. */
  const std::vector<size_t>& getDimensions() const { return dimensions; }

  /**
   * Check if this is an unsized array (array&lt;T&gt; without dimensions)
   */
  bool isUnsized() const { return dimensions.empty(); }

  /**
   * Get total number of elements (product of all dimensions)
   * Returns 0 for unsized arrays
   */
  size_t getTotalElements() const {
    if (isUnsized()) return 0;
    return std::accumulate(dimensions.begin(), dimensions.end(), size_t{1},
                           std::multiplies<size_t>());
  }

  /**
   * Check if this is a 1D array
   */
  bool is1D() const { return dimensions.size() == 1; }

  /**
   * Get the innermost element type (for nested arrays, recurse)
   */
  TypePtr getInnermostType() const {
    if (auto* inner = dynamic_cast<const ArrayType*>(elementType.get())) {
      return inner->getInnermostType();
    }
    return elementType;
  }

  /**
   * Get the type after indexing once (removes outermost dimension)
   * For array<i32, 3, 2>[i] -> array<i32, 2>
   * For array<i32, 5>[i] -> i32
   */
  TypePtr getIndexedType() const {
    if (dimensions.size() == 1) {
      return elementType;
    }
    // Create new array type with remaining dimensions
    std::vector<size_t> remainingDims(dimensions.begin() + 1, dimensions.end());
    return std::make_shared<ArrayType>(elementType, std::move(remainingDims));
  }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = "array<" + elementType->toString();
    for (size_t dim : dimensions) {
      result += ", " + std::to_string(dim);
    }
    result += ">";
    return result;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    std::string result = "array<" + elementType->toDisplayString();
    for (size_t dim : dimensions) {
      result += ", " + std::to_string(dim);
    }
    result += ">";
    return result;
  }

  /**
   * Check if a sized array is compatible with this type
   * Used for coercion from array<T, m, n> to array&lt;T&gt; (unsized)
   */
  bool isCompatibleWith(const ArrayType& other) const {
    if (!elementType->equals(*other.elementType)) return false;
    // Unsized array accepts any sized array with same element type
    if (isUnsized()) return true;
    // If this is sized, other must match exactly
    if (dimensions.size() != other.dimensions.size()) return false;
    for (size_t i = 0; i < dimensions.size(); ++i) {
      if (dimensions[i] != other.dimensions[i]) return false;
    }
    return true;
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* a = dynamic_cast<const ArrayType*>(&other)) {
      if (!elementType->equals(*a->elementType)) return false;
      // Either both unsized, or dimensions match exactly
      if (isUnsized() && a->isUnsized()) return true;
      if (dimensions.size() != a->dimensions.size()) return false;
      for (size_t i = 0; i < dimensions.size(); ++i) {
        if (dimensions[i] != a->dimensions[i]) return false;
      }
      return true;
    }
    return false;
  }

  /**
   * The view struct { ptr data, i32 ndims, ptr dims } that a `ref array<T>`
   * is carried as: the element storage, the rank, and the dimension sizes
   * (an i64 table that outlives the view - a private constant global).
   */
  static llvm::StructType* getArrayStructType(llvm::LLVMContext& ctx) {
    // Check for existing named type to avoid duplicates
    if (auto* existing = llvm::StructType::getTypeByName(
            ctx, sun::semantic_analysis::ArrayStruct)) {
      return existing;
    }
    return llvm::StructType::create(
        ctx,
        {
            llvm::PointerType::getUnqual(ctx),  // data ptr
            llvm::Type::getInt32Ty(ctx),        // ndims
            llvm::PointerType::getUnqual(ctx)   // dims ptr (points to i64[])
        },
        sun::semantic_analysis::ArrayStruct);
  }

  /**
   * A sized array is its inline storage; an unsized one is the view struct
   * (reached only through ReferenceType::toLLVMType).
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    if (isUnsized()) return getArrayStructType(ctx);
    return getDataStorageType(ctx);
  }

  /**
   * The inline storage type of a sized array: [3 x [2 x i32]] for
   * array<i32, 3, 2>. Null for an unsized array.
   */
  llvm::Type* getDataStorageType(llvm::LLVMContext& ctx) const {
    if (isUnsized()) {
      // Unsized arrays have no fixed storage type
      return nullptr;
    }
    llvm::Type* result = elementType->toLLVMType(ctx);
    // Build from innermost to outermost
    for (auto it = dimensions.rbegin(); it != dimensions.rend(); ++it) {
      result = llvm::ArrayType::get(result, *it);
    }
    return result;
  }

  /**
   * Get the LLVM element type
   */
  llvm::Type* getElementLLVMType(llvm::LLVMContext& ctx) const {
    return elementType->toLLVMType(ctx);
  }
};

}  // namespace sun::types
