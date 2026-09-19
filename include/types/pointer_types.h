/** Describes raw pointers, static pointers, and the null value. */
#pragma once

#include "llvm/IR/DerivedTypes.h"
#include "semantic_analysis/struct_names.h"
#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * Raw pointer type - non-owning pointer for C interop (no automatic cleanup)
 * Type annotation: raw_ptr&lt;T&gt; where T is the pointee type
 * Examples: raw_ptr<i8> = char*, raw_ptr<raw_ptr<i8>> = char** for argv
 */
class RawPointerType : public Type {
  TypePtr pointeeType;  // The type being pointed to

 public:
  /** Creates a non-owning raw pointer type for the supplied pointee type. */
  explicit RawPointerType(TypePtr pointee) : pointeeType(std::move(pointee)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::RawPointer;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the pointee type stored by this object. */
  const TypePtr& getPointeeType() const { return pointeeType; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return "raw_ptr(" + pointeeType->toString() + ")";
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    return "raw_ptr<" + pointeeType->toDisplayString() + ">";
  }

  /**
   * Defined out-of-line below (needs StaticPointerType to be complete)
   */
  bool equals(const Type& other) const override;

  /**
   * Returns opaque pointer in modern LLVM (all pointers are ptr)
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::PointerType::getUnqual(ctx);
  }

  /**
   * Get the LLVM type of the pointee (for load/store operations)
   */
  llvm::Type* getPointeeLLVMType(llvm::LLVMContext& ctx) const {
    return pointeeType->toLLVMType(ctx);
  }
};

/**
 * Static pointer type - pointer to immortal static data (string literals,
 * globals) Type annotation: static_ptr&lt;T&gt; where T is the pointee type
 * Memory safe: never freed, always valid, read-only Represented as a fat
 * pointer struct: { ptr data, i64 length } Can implicitly convert to
 * raw_ptr&lt;T&gt; for function calls (extracts data ptr)
 */
class StaticPointerType : public Type {
  TypePtr pointeeType;  // The type being pointed to
  mutable llvm::StructType* cachedLLVMType = nullptr;

 public:
  /** Creates a pointer type referring to storage with static lifetime. */
  explicit StaticPointerType(TypePtr pointee)
      : pointeeType(std::move(pointee)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::StaticPointer;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the pointee type stored by this object. */
  const TypePtr& getPointeeType() const { return pointeeType; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return "static_ptr(" + pointeeType->toString() + ")";
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    return "static_ptr<" + pointeeType->toDisplayString() + ">";
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    // Static pointer is compatible with null
    if (other.isNullPointer()) return true;
    // A raw_ptr does NOT satisfy a static_ptr: the fat pointer needs a length
    // and a promise the bytes are immortal, and a bare pointer carries
    // neither. The other direction (a static_ptr where raw_ptr is expected)
    // lives in RawPointerType::equals — narrowing loses nothing.
    if (auto* p = dynamic_cast<const StaticPointerType*>(&other)) {
      return pointeeType->equals(*p->pointeeType);
    }
    return false;
  }

  /**
   * Returns fat pointer struct: { ptr data, i64 length }
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    if (!cachedLLVMType) {
      // Check if the type already exists in the context (avoids creating
      // duplicates like static_ptr_struct.0, static_ptr_struct.1, etc.)
      cachedLLVMType = llvm::StructType::getTypeByName(
          ctx, sun::semantic_analysis::StaticPtr);
      if (!cachedLLVMType) {
        cachedLLVMType = llvm::StructType::create(
            ctx,
            {llvm::PointerType::getUnqual(ctx), llvm::Type::getInt64Ty(ctx)},
            sun::semantic_analysis::StaticPtr);
      }
    }
    return cachedLLVMType;
  }

  /**
   * Get the LLVM struct type for the fat pointer
   */
  llvm::StructType* getStructType(llvm::LLVMContext& ctx) const {
    toLLVMType(ctx);  // Ensure it's created
    return cachedLLVMType;
  }

  /**
   * Get the LLVM type of the pointee (for load/store operations)
   */
  llvm::Type* getPointeeLLVMType(llvm::LLVMContext& ctx) const {
    return pointeeType->toLLVMType(ctx);
  }
};

/**
 * Null pointer type - represents the null literal
 * Compatible with any pointer type for assignment and comparison
 */
class NullPointerType : public Type {
 public:
  /** Creates an instance with its default state. */
  NullPointerType() = default;

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::NullPointer;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "null"; }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    // Null is equal to itself
    if (other.isNullPointer()) return true;
    // Null is also "equal" (compatible) with any pointer type
    if (other.isRawPointer() || other.isStaticPointer()) return true;
    return false;
  }

  /**
   * Returns opaque pointer (null is a pointer value)
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::PointerType::getUnqual(ctx);
  }
};

}  // namespace sun::types
