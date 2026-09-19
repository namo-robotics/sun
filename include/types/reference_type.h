/** Describes mutable and const borrows and their lifetime metadata. */
#pragma once

#include <vector>

#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * Reference type - behaves like a pointer but with implicit dereferencing
 * Type annotation: ref(T) where T is the referenced type
 * Examples: ref(i32) = reference to i32
 * When reading a reference, it automatically dereferences
 * When assigning to a reference, it stores to the underlying address
 */
class ReferenceType : public Type {
  TypePtr referencedType;  // The type being referenced
  bool mutable_;           // true = mutable ref, false = immutable ref
  // Metadata, NOT identity: the lifetime name written on this position
  // ('ref 'a Bus'), empty when elided. Ignored by equals() and toString(),
  // exactly like LambdaType's - see the comment there.
  std::string lifetimeName_;
  std::vector<std::string> classLifetimeArgs_;

 public:
  /** Creates a borrowed reference type with the requested mutability. */
  explicit ReferenceType(TypePtr referenced, bool isMutable = true)
      : referencedType(std::move(referenced)), mutable_(isMutable) {}

  /** Returns the named lifetime associated with the borrowed value. */
  const std::string& getLifetimeName() const { return lifetimeName_; }
  /** Assigns the named lifetime associated with the borrowed value. */
  void setLifetimeName(std::string name) { lifetimeName_ = std::move(name); }
  /**
   * Lifetime arguments applied to the referent's class ('ref Bus<'this>'):
   * positionally binding the class's declared lifetimes. Metadata like
   * lifetimeName_ - never part of the type's identity.
   */
  const std::vector<std::string>& getClassLifetimeArgs() const {
    return classLifetimeArgs_;
  }
  /** Updates the lifetime arguments for the referenced class. */
  void setClassLifetimeArgs(std::vector<std::string> args) {
    classLifetimeArgs_ = std::move(args);
  }

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Reference;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the referenced type stored by this object. */
  const TypePtr& getReferencedType() const { return referencedType; }
  /** Reports whether the loan grants exclusive write access to its target. */
  bool isMutable() const { return mutable_; }

  /**
   * Check if this is a reference to an unsized array
   */
  bool isUnsizedArrayRef() const;

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return std::string(mutable_ ? "ref(" : "const ref(") +
           referencedType->toString() + ")";
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    return std::string(mutable_ ? "ref " : "const ref ") +
           referencedType->toDisplayString();
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* r = dynamic_cast<const ReferenceType*>(&other)) {
      return referencedType->equals(*r->referencedType) &&
             mutable_ == r->mutable_;
    }
    return false;
  }

  /**
   * Returns opaque pointer or fat pointer struct for unsized array refs
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override;

  /**
   * Get the LLVM type of the referenced value (for load/store operations)
   */
  llvm::Type* getReferencedLLVMType(llvm::LLVMContext& ctx) const {
    return referencedType->toLLVMType(ctx);
  }
};

}  // namespace sun::types
