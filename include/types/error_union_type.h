/** Describes values paired with an error alternative. */
#pragma once

#include "llvm/IR/DerivedTypes.h"
#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * Error union type - represents a type that can be either a value or an error
 * Following Zig's model where errors are values
 * Represented as a struct { bool isError; union { ValueType value; i32
 * errorCode; } } For simplicity, we use { i1 isError, T value } where we check
 * isError first
 */
class ErrorUnionType : public Type {
  TypePtr valueType;  // The non-error type (e.g., i32 in "i32, error")

 public:
  /** Creates a result type that can hold either a value or an error. */
  explicit ErrorUnionType(TypePtr value) : valueType(std::move(value)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::ErrorUnion;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the value type stored by this object. */
  const TypePtr& getValueType() const { return valueType; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return valueType->toString() + ", error";
  }

  /**
   * Spelled the way the source spells it: "i32 throws IError"
   */
  std::string toDisplayString() const override {
    return valueType->toDisplayString() + " throws IError";
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* e = dynamic_cast<const ErrorUnionType*>(&other)) {
      return valueType->equals(*e->valueType);
    }
    return false;
  }

  /**
   * Error union is represented as a struct: { i1 isError, &lt;valueType&gt;
   * value } If isError is true, the error code is stored in the value field (as
   * i64)
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    // Create a struct type: { i1, valueType }
    // The first field indicates whether this is an error
    // The second field holds either the value or the error code
    std::vector<llvm::Type*> fields;
    fields.push_back(llvm::Type::getInt1Ty(ctx));  // isError flag

    // For the payload, use the larger of valueType or i64 (for error codes)
    // For simplicity, we'll store the value type and handle error codes
    // specially
    fields.push_back(valueType->toLLVMType(ctx));

    return llvm::StructType::get(ctx, fields);
  }

  /**
   * Get the LLVM type of the value (for extracting the value when not an error)
   */
  llvm::Type* getValueLLVMType(llvm::LLVMContext& ctx) const {
    return valueType->toLLVMType(ctx);
  }
};

}  // namespace sun::types
