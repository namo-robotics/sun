// types.h — Type system for Sun language

#pragma once

#include <cassert>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "error.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "struct_names.h"

namespace sun {

// Forward declarations
class Type;
class ArrayType;  // Forward declared for ReferenceType
using TypePtr = std::shared_ptr<Type>;

// Base Type class
class Type {
 public:
  enum class Kind {
    // Primitive types
    Void,
    Bool,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,    // u8
    UInt16,   // u16
    UInt32,   // u32
    UInt64,   // u64
    Float32,  // f32
    Float64,  // f64 (current "double")
    // Composite types
    Function,
    Lambda,
    RawPointer,     // Non-owning raw pointer for C interop (raw_ptr<T>)
    StaticPointer,  // Pointer to immortal static data (static_ptr<T>)
    NullPointer,  // Special type for null literal - compatible with any pointer
    Reference,    // Reference type - like pointer but with implicit deref
    Class,
    Interface,
    Enum,           // Enum type: enum Color { Red, Green, Blue }
    TypeParameter,  // Generic type parameter (T, U, etc.)
    ErrorUnion,     // Type that can be a value or an error
    Array,          // Fixed-size array: array<T, N, M, ...>
    Slice,          // Builtin slice type: { start: i64, end: i64 }
    Module          // Module/namespace reference (for mod_x.mod_y.var access)
  };

  virtual ~Type() = default;
  virtual Kind getKind() const = 0;
  virtual std::string toString() const = 0;
  virtual bool equals(const Type& other) const = 0;
  virtual llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const = 0;

  // Convenience checks for primitive types
  bool isVoid() const { return getKind() == Kind::Void; }
  bool isBool() const { return getKind() == Kind::Bool; }
  bool isInt8() const { return getKind() == Kind::Int8; }
  bool isInt16() const { return getKind() == Kind::Int16; }
  bool isInt32() const { return getKind() == Kind::Int32; }
  bool isInt64() const { return getKind() == Kind::Int64; }
  bool isUInt8() const { return getKind() == Kind::UInt8; }
  bool isUInt16() const { return getKind() == Kind::UInt16; }
  bool isUInt32() const { return getKind() == Kind::UInt32; }
  bool isUInt64() const { return getKind() == Kind::UInt64; }
  bool isFloat32() const { return getKind() == Kind::Float32; }
  bool isFloat64() const { return getKind() == Kind::Float64; }
  bool isSigned() const {
    Kind k = getKind();
    return k == Kind::Int8 || k == Kind::Int16 || k == Kind::Int32 ||
           k == Kind::Int64;
  }
  bool isUnsigned() const {
    Kind k = getKind();
    return k == Kind::UInt8 || k == Kind::UInt16 || k == Kind::UInt32 ||
           k == Kind::UInt64;
  }
  bool isPrimitive() const {
    Kind k = getKind();
    return k == Kind::Void || k == Kind::Bool || k == Kind::Int8 ||
           k == Kind::Int16 || k == Kind::Int32 || k == Kind::Int64 ||
           k == Kind::UInt8 || k == Kind::UInt16 || k == Kind::UInt32 ||
           k == Kind::UInt64 || k == Kind::Float32 || k == Kind::Float64;
  }
  // Convenience checks for composite types
  bool isFunction() const { return getKind() == Kind::Function; }
  bool isLambda() const { return getKind() == Kind::Lambda; }
  bool isRawPointer() const { return getKind() == Kind::RawPointer; }
  bool isStaticPointer() const { return getKind() == Kind::StaticPointer; }
  bool isNullPointer() const { return getKind() == Kind::NullPointer; }
  bool isReference() const { return getKind() == Kind::Reference; }

  // Returns true for any pointer-like type (raw or static)
  bool isAnyPointer() const { return isRawPointer() || isStaticPointer(); }
  bool isClass() const { return getKind() == Kind::Class; }
  bool isInterface() const { return getKind() == Kind::Interface; }
  bool isEnum() const { return getKind() == Kind::Enum; }
  bool isModule() const { return getKind() == Kind::Module; }
  bool isTypeParameter() const { return getKind() == Kind::TypeParameter; }
  bool isErrorUnion() const { return getKind() == Kind::ErrorUnion; }
  bool isArray() const { return getKind() == Kind::Array; }
  bool isSlice() const { return getKind() == Kind::Slice; }
  bool isCallable() const { return isFunction() || isLambda(); }
  // Compound types must be passed by reference (classes, interfaces, arrays)
  // Note: Enums are NOT compound - they are i32 values and passed by value
  bool isCompound() const {
    return !isPrimitive() && !isReference() && !isRawPointer() &&
           !isStaticPointer() && !isFunction() && !isLambda() &&
           !isTypeParameter() && !isEnum();
  }
  bool isNumeric() const;
  bool isIntegral() const;
  bool isFloatingPoint() const;
  bool isString() const;
};

// Type parameter (used in generic class/function definitions)
// Represents a type variable like T, U, V in class List<T>
class TypeParameterType : public Type {
  std::string name;  // Parameter name: T, U, etc.

 public:
  explicit TypeParameterType(std::string paramName)
      : name(std::move(paramName)) {}

  Kind getKind() const override { return Kind::TypeParameter; }
  const std::string& getName() const { return name; }

  std::string toString() const override { return name; }

  bool equals(const Type& other) const override {
    if (auto* p = dynamic_cast<const TypeParameterType*>(&other))
      return name == p->name;
    return false;
  }

  // Type parameters can't be directly converted to LLVM types
  // They must be substituted first
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    // This should never be called for unsubstituted type parameters
    assert(false && "Cannot convert type parameter to LLVM type");
    return nullptr;
  }
};

// Primitive types (int, float, bool, etc.)
class PrimitiveType : public Type {
  Kind kind;

 public:
  explicit PrimitiveType(Kind k) : kind(k) {}

  Kind getKind() const override { return kind; }

  std::string toString() const override {
    switch (kind) {
      case Kind::Void:
        return "void";
      case Kind::Bool:
        return "bool";
      case Kind::Int8:
        return "i8";
      case Kind::Int16:
        return "i16";
      case Kind::Int32:
        return "i32";
      case Kind::Int64:
        return "i64";
      case Kind::UInt8:
        return "u8";
      case Kind::UInt16:
        return "u16";
      case Kind::UInt32:
        return "u32";
      case Kind::UInt64:
        return "u64";
      case Kind::Float32:
        return "f32";
      case Kind::Float64:
        return "f64";
      default:
        return "unknown";
    }
  }

  bool equals(const Type& other) const override {
    return kind == other.getKind();
  }

  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    switch (kind) {
      case Kind::Void:
        return llvm::Type::getVoidTy(ctx);
      case Kind::Bool:
        return llvm::Type::getInt1Ty(ctx);
      case Kind::Int8:
        return llvm::Type::getInt8Ty(ctx);
      case Kind::Int16:
        return llvm::Type::getInt16Ty(ctx);
      case Kind::Int32:
        return llvm::Type::getInt32Ty(ctx);
      case Kind::Int64:
        return llvm::Type::getInt64Ty(ctx);
      case Kind::UInt8:
        return llvm::Type::getInt8Ty(
            ctx);  // Same LLVM type, signedness is semantic
      case Kind::UInt16:
        return llvm::Type::getInt16Ty(ctx);
      case Kind::UInt32:
        return llvm::Type::getInt32Ty(ctx);
      case Kind::UInt64:
        return llvm::Type::getInt64Ty(ctx);
      case Kind::Float32:
        return llvm::Type::getFloatTy(ctx);
      case Kind::Float64:
        return llvm::Type::getDoubleTy(ctx);
      default:
        return nullptr;
    }
  }
};

// Function type for named functions (direct call, not returnable)
// Type annotation: _() -> {}
class FunctionType : public Type {
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;

 public:
  FunctionType(TypePtr ret, std::vector<TypePtr> params)
      : returnType(std::move(ret)), paramTypes(std::move(params)) {}

  Kind getKind() const override { return Kind::Function; }
  const TypePtr& getReturnType() const { return returnType; }
  const std::vector<TypePtr>& getParamTypes() const { return paramTypes; }

  std::string toString() const override {
    std::string result = "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toString();
    }
    result += ") -> " + returnType->toString();
    return result;
  }

  bool equals(const Type& other) const override {
    if (auto* f = dynamic_cast<const FunctionType*>(&other)) {
      if (!returnType->equals(*f->returnType)) return false;
      if (paramTypes.size() != f->paramTypes.size()) return false;
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (!paramTypes[i]->equals(*f->paramTypes[i])) return false;
      }
      return true;
    }
    return false;
  }

  // Returns the LLVM FunctionType (not a pointer)
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    std::vector<llvm::Type*> llvmParams;
    for (const auto& p : paramTypes) {
      llvmParams.push_back(p->toLLVMType(ctx));
    }
    return llvm::FunctionType::get(returnType->toLLVMType(ctx), llvmParams,
                                   false);
  }

  // Returns a pointer to the function type (for function pointer variables)
  llvm::Type* toPointerType(llvm::LLVMContext& ctx) const {
    return llvm::PointerType::getUnqual(toLLVMType(ctx));
  }

  // Get the raw LLVM FunctionType (for indirect calls)
  llvm::FunctionType* toLLVMFunctionType(llvm::LLVMContext& ctx) const {
    std::vector<llvm::Type*> llvmParams;
    for (const auto& p : paramTypes) {
      llvmParams.push_back(p->toLLVMType(ctx));
    }
    return llvm::FunctionType::get(returnType->toLLVMType(ctx), llvmParams,
                                   false);
  }

  // Get the closure struct type { ptr, ptr } (func*, env*)
  llvm::StructType* toLLVMClosureType(llvm::LLVMContext& ctx) const {
    return llvm::StructType::get(
        ctx, {
                 llvm::PointerType::getUnqual(ctx),  // func*
                 llvm::PointerType::getUnqual(ctx)   // env*
             });
  }
};

// Lambda type for anonymous functions (fat pointer call, returnable)
// Type annotation: () -> {}
class LambdaType : public Type {
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;

 public:
  LambdaType(TypePtr ret, std::vector<TypePtr> params)
      : returnType(std::move(ret)), paramTypes(std::move(params)) {}

  Kind getKind() const override { return Kind::Lambda; }
  const TypePtr& getReturnType() const { return returnType; }
  const std::vector<TypePtr>& getParamTypes() const { return paramTypes; }

  std::string toString() const override {
    std::string result = "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toString();
    }
    result += ") -> " + returnType->toString();
    return result;
  }

  bool equals(const Type& other) const override {
    if (auto* l = dynamic_cast<const LambdaType*>(&other)) {
      if (!returnType->equals(*l->returnType)) return false;
      if (paramTypes.size() != l->paramTypes.size()) return false;
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (!paramTypes[i]->equals(*l->paramTypes[i])) return false;
      }
      return true;
    }
    return false;
  }

  // Returns the closure struct type { ptr, ptr } (func*, env*)
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::StructType::get(
        ctx, {
                 llvm::PointerType::getUnqual(ctx),  // func*
                 llvm::PointerType::getUnqual(ctx)   // env*
             });
  }

  // Get the raw LLVM FunctionType (for the actual function signature with
  // closure param)
  llvm::FunctionType* toLLVMFunctionType(llvm::LLVMContext& ctx) const {
    std::vector<llvm::Type*> llvmParams;
    // First param is the fat pointer (closure struct pointer)
    llvmParams.push_back(llvm::PointerType::getUnqual(ctx));
    for (const auto& p : paramTypes) {
      llvmParams.push_back(p->toLLVMType(ctx));
    }
    return llvm::FunctionType::get(returnType->toLLVMType(ctx), llvmParams,
                                   false);
  }
};

// Forward declaration for cross-reference in equals()
class StaticPointerType;

// Raw pointer type - non-owning pointer for C interop (no automatic cleanup)
// Type annotation: raw_ptr<T> where T is the pointee type
// Examples: raw_ptr<i8> = char*, raw_ptr<raw_ptr<i8>> = char** for argv
class RawPointerType : public Type {
  TypePtr pointeeType;  // The type being pointed to

 public:
  explicit RawPointerType(TypePtr pointee) : pointeeType(std::move(pointee)) {}

  Kind getKind() const override { return Kind::RawPointer; }
  const TypePtr& getPointeeType() const { return pointeeType; }

  std::string toString() const override {
    return "raw_ptr(" + pointeeType->toString() + ")";
  }

  // Defined out-of-line below (needs StaticPointerType to be complete)
  bool equals(const Type& other) const override;

  // Returns opaque pointer in modern LLVM (all pointers are ptr)
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::PointerType::getUnqual(ctx);
  }

  // Get the LLVM type of the pointee (for load/store operations)
  llvm::Type* getPointeeLLVMType(llvm::LLVMContext& ctx) const {
    return pointeeType->toLLVMType(ctx);
  }
};

// Static pointer type - pointer to immortal static data (string literals,
// globals) Type annotation: static_ptr<T> where T is the pointee type Memory
// safe: never freed, always valid, read-only
// Represented as a fat pointer struct: { ptr data, i64 length }
// Can implicitly convert to raw_ptr<T> for function calls (extracts data ptr)
class StaticPointerType : public Type {
  TypePtr pointeeType;  // The type being pointed to
  mutable llvm::StructType* cachedLLVMType = nullptr;

 public:
  explicit StaticPointerType(TypePtr pointee)
      : pointeeType(std::move(pointee)) {}

  Kind getKind() const override { return Kind::StaticPointer; }
  const TypePtr& getPointeeType() const { return pointeeType; }

  std::string toString() const override {
    return "static_ptr(" + pointeeType->toString() + ")";
  }

  bool equals(const Type& other) const override {
    // Static pointer is compatible with null
    if (other.isNullPointer()) return true;
    // Static pointer is compatible with raw_ptr (implicit conversion)
    if (auto* r = dynamic_cast<const RawPointerType*>(&other)) {
      return pointeeType->equals(*r->getPointeeType());
    }
    if (auto* p = dynamic_cast<const StaticPointerType*>(&other)) {
      return pointeeType->equals(*p->pointeeType);
    }
    return false;
  }

  // Returns fat pointer struct: { ptr data, i64 length }
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    if (!cachedLLVMType) {
      cachedLLVMType = llvm::StructType::create(
          ctx, {llvm::PointerType::getUnqual(ctx), llvm::Type::getInt64Ty(ctx)},
          sun::StructNames::StaticPtr);
    }
    return cachedLLVMType;
  }

  // Get the LLVM struct type for the fat pointer
  llvm::StructType* getStructType(llvm::LLVMContext& ctx) const {
    toLLVMType(ctx);  // Ensure it's created
    return cachedLLVMType;
  }

  // Get the LLVM type of the pointee (for load/store operations)
  llvm::Type* getPointeeLLVMType(llvm::LLVMContext& ctx) const {
    return pointeeType->toLLVMType(ctx);
  }
};

// Null pointer type - represents the null literal
// Compatible with any pointer type for assignment and comparison
class NullPointerType : public Type {
 public:
  NullPointerType() = default;

  Kind getKind() const override { return Kind::NullPointer; }

  std::string toString() const override { return "null"; }

  bool equals(const Type& other) const override {
    // Null is equal to itself
    if (other.isNullPointer()) return true;
    // Null is also "equal" (compatible) with any pointer type
    if (other.isRawPointer() || other.isStaticPointer()) return true;
    return false;
  }

  // Returns opaque pointer (null is a pointer value)
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::PointerType::getUnqual(ctx);
  }
};

// Reference type - behaves like a pointer but with implicit dereferencing
// Type annotation: ref(T) where T is the referenced type
// Examples: ref(i32) = reference to i32
// When reading a reference, it automatically dereferences
// When assigning to a reference, it stores to the underlying address
class ReferenceType : public Type {
  TypePtr referencedType;  // The type being referenced
  bool mutable_;           // true = mutable ref, false = immutable ref

 public:
  explicit ReferenceType(TypePtr referenced, bool isMutable = true)
      : referencedType(std::move(referenced)), mutable_(isMutable) {}

  Kind getKind() const override { return Kind::Reference; }
  const TypePtr& getReferencedType() const { return referencedType; }
  bool isMutable() const { return mutable_; }

  // Check if this is a reference to an unsized array
  // Implemented after ArrayType definition
  inline bool isUnsizedArrayRef() const;

  std::string toString() const override {
    if (mutable_) {
      return "ref(" + referencedType->toString() + ")";
    }
    return "ref const(" + referencedType->toString() + ")";
  }

  bool equals(const Type& other) const override {
    if (auto* r = dynamic_cast<const ReferenceType*>(&other)) {
      return referencedType->equals(*r->referencedType) &&
             mutable_ == r->mutable_;
    }
    return false;
  }

  // Returns opaque pointer or fat pointer struct for unsized array refs
  // Implemented after ArrayType definition
  inline llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override;

  // Get the LLVM type of the referenced value (for load/store operations)
  llvm::Type* getReferencedLLVMType(llvm::LLVMContext& ctx) const {
    return referencedType->toLLVMType(ctx);
  }
};

// Helper: unwrap reference types (ref(T) -> T, otherwise unchanged)
// References should behave like values, transparently dereferenced
inline TypePtr unwrapRef(TypePtr type) {
  if (type && type->isReference()) {
    return static_cast<const ReferenceType*>(type.get())->getReferencedType();
  }
  return type;
}

// Error union type - represents a type that can be either a value or an error
// Following Zig's model where errors are values
// Represented as a struct { bool isError; union { ValueType value; i32
// errorCode; } } For simplicity, we use { i1 isError, T value } where we check
// isError first
class ErrorUnionType : public Type {
  TypePtr valueType;  // The non-error type (e.g., i32 in "i32, error")

 public:
  explicit ErrorUnionType(TypePtr value) : valueType(std::move(value)) {}

  Kind getKind() const override { return Kind::ErrorUnion; }
  const TypePtr& getValueType() const { return valueType; }

  std::string toString() const override {
    return valueType->toString() + ", error";
  }

  bool equals(const Type& other) const override {
    if (auto* e = dynamic_cast<const ErrorUnionType*>(&other)) {
      return valueType->equals(*e->valueType);
    }
    return false;
  }

  // Error union is represented as a struct: { i1 isError, <valueType> value }
  // If isError is true, the error code is stored in the value field (as i64)
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

  // Get the LLVM type of the value (for extracting the value when not an error)
  llvm::Type* getValueLLVMType(llvm::LLVMContext& ctx) const {
    return valueType->toLLVMType(ctx);
  }
};

// Fixed-size array type: array<T, N> or array<T, M, N> for multi-dimensional
// Type annotation: array<i32, 5> for [5 x i32], array<i32, 3, 2> for [3 x [2 x
// i32]] Stack-allocated, bounds-checked at compile-time for constant indices
// Unsized arrays: array<T> (empty dimensions) accept any array<T, ...>
class ArrayType : public Type {
  TypePtr elementType;             // The element type (e.g., i32)
  std::vector<size_t> dimensions;  // Fixed sizes (e.g., {5} or {3, 2}), empty
                                   // means unsized

 public:
  ArrayType(TypePtr elemType, std::vector<size_t> dims)
      : elementType(std::move(elemType)), dimensions(std::move(dims)) {}

  Kind getKind() const override { return Kind::Array; }
  const TypePtr& getElementType() const { return elementType; }
  const std::vector<size_t>& getDimensions() const { return dimensions; }

  // Check if this is an unsized array (array<T> without dimensions)
  bool isUnsized() const { return dimensions.empty(); }

  // Get total number of elements (product of all dimensions)
  // Returns 0 for unsized arrays
  size_t getTotalElements() const {
    if (isUnsized()) return 0;
    return std::accumulate(dimensions.begin(), dimensions.end(), size_t{1},
                           std::multiplies<size_t>());
  }

  // Check if this is a 1D array
  bool is1D() const { return dimensions.size() == 1; }

  // Get the innermost element type (for nested arrays, recurse)
  TypePtr getInnermostType() const {
    if (auto* inner = dynamic_cast<const ArrayType*>(elementType.get())) {
      return inner->getInnermostType();
    }
    return elementType;
  }

  // Get the type after indexing once (removes outermost dimension)
  // For array<i32, 3, 2>[i] -> array<i32, 2>
  // For array<i32, 5>[i] -> i32
  TypePtr getIndexedType() const {
    if (dimensions.size() == 1) {
      return elementType;
    }
    // Create new array type with remaining dimensions
    std::vector<size_t> remainingDims(dimensions.begin() + 1, dimensions.end());
    return std::make_shared<ArrayType>(elementType, std::move(remainingDims));
  }

  std::string toString() const override {
    std::string result = "array<" + elementType->toString();
    for (size_t dim : dimensions) {
      result += ", " + std::to_string(dim);
    }
    result += ">";
    return result;
  }

  // Check if a sized array is compatible with this type
  // Used for coercion from array<T, m, n> to array<T> (unsized)
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

  // Get the fat array struct type: { ptr data, i32 ndims, ptr dims }
  // ALL arrays (sized and unsized) use this representation
  // - data: pointer to the contiguous element storage
  // - ndims: number of dimensions
  // - dims: pointer to i64 array of dimension sizes
  static llvm::StructType* getArrayStructType(llvm::LLVMContext& ctx) {
    // Check for existing named type to avoid duplicates
    if (auto* existing = llvm::StructType::getTypeByName(
            ctx, sun::StructNames::ArrayStruct)) {
      return existing;
    }
    return llvm::StructType::create(
        ctx,
        {
            llvm::PointerType::getUnqual(ctx),  // data ptr
            llvm::Type::getInt32Ty(ctx),        // ndims
            llvm::PointerType::getUnqual(ctx)   // dims ptr (points to i64[])
        },
        sun::StructNames::ArrayStruct);
  }

  // Array is represented as a fat struct: { ptr data, i32 ndims, ptr dims }
  // This allows arrays of any shape to be passed around uniformly
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return getArrayStructType(ctx);
  }

  // Get the raw LLVM array type for the data storage (used internally)
  // For array<i32, 3, 2> this returns [3 x [2 x i32]]
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

  // Get the LLVM element type
  llvm::Type* getElementLLVMType(llvm::LLVMContext& ctx) const {
    return elementType->toLLVMType(ctx);
  }
};

// Slice type: builtin struct for array/matrix indexing
// Represents a range [start, end) or a single index (when end = start + 1)
// Used with IIndexable interface for uniform slice handling
// LLVM representation: { i64 start, i64 end }
class SliceType : public Type {
 public:
  SliceType() = default;

  Kind getKind() const override { return Kind::Slice; }

  std::string toString() const override { return "slice"; }

  bool equals(const Type& other) const override { return other.isSlice(); }

  // Get the LLVM struct type for slice: { i64, i64 }
  static llvm::StructType* getSliceStructType(llvm::LLVMContext& ctx) {
    return llvm::StructType::get(ctx,
                                 {
                                     llvm::Type::getInt64Ty(ctx),  // start
                                     llvm::Type::getInt64Ty(ctx)   // end
                                 });
  }

  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return getSliceStructType(ctx);
  }

  // Helper to check if a slice is a single index (end == start + 1)
  // This is used to distinguish m[5] from m[5:6]
  static bool isSingleIndex(int64_t start, int64_t end) {
    return end == start + 1;
  }
};

// Module/namespace reference type
// Used in semantic analysis when accessing module-scoped variables/functions
// e.g., in "mod_x.mod_y.a", mod_x and mod_x.mod_y have ModuleType
class ModuleType : public Type {
  std::string modulePath;  // e.g., "mod_x" or "$hash$_mod_x"

 public:
  explicit ModuleType(std::string path) : modulePath(std::move(path)) {}

  Kind getKind() const override { return Kind::Module; }
  const std::string& getModulePath() const { return modulePath; }

  std::string toString() const override { return "module<" + modulePath + ">"; }

  bool equals(const Type& other) const override {
    if (!other.isModule()) return false;
    return modulePath == static_cast<const ModuleType&>(other).modulePath;
  }

  // Module types don't have LLVM representation - they're resolved at compile
  // time
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return nullptr;
  }
};

// ReferenceType method implementations (after ArrayType is defined)
inline bool ReferenceType::isUnsizedArrayRef() const {
  if (referencedType->isArray()) {
    return static_cast<const ArrayType*>(referencedType.get())->isUnsized();
  }
  return false;
}

// ref array<T> is just a pointer to the fat array struct
inline llvm::Type* ReferenceType::toLLVMType(llvm::LLVMContext& ctx) const {
  return llvm::PointerType::getUnqual(ctx);
}

// Class field information
struct ClassField {
  std::string name;
  TypePtr type;
  size_t index;  // Index in the struct
};

// Class method information
struct ClassMethod {
  std::string name;
  std::vector<std::string> typeParameters;  // Generic type params: <T, U>
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;  // Excludes implicit 'this' parameter
  bool isConstructor;               // true if this is the 'init' method

  bool isGeneric() const { return !typeParameters.empty(); }
};

// Class type for user-defined classes
// Classes are represented as LLVM structs with methods as separate functions
// Generic classes have type parameters (e.g., class List<T>)
// Specialized classes have type arguments (e.g., List<i32>)
class ClassType : public Type {
  std::string name;  // Fully qualified name (e.g., "$hash$_sun_Vec")
  std::string
      baseName_;  // User-written base name (e.g., "Unique") for error messages
  std::vector<std::string>
      typeParameters;  // Type params: ["T", "U"] for generic definitions
  std::vector<TypePtr>
      typeArguments;            // Type args: [i32] for specialized classes
  std::string baseGenericName;  // For specialized: original generic class name
  std::vector<ClassField> fields;
  std::vector<ClassMethod> methods;
  std::vector<std::string>
      implementedInterfaces;  // Names of interfaces this class implements
  mutable llvm::StructType* cachedLLVMType = nullptr;

 public:
  ClassType(std::string className) : name(std::move(className)) {}

  // Constructor for generic class definition
  ClassType(std::string className, std::vector<std::string> typeParams)
      : name(std::move(className)), typeParameters(std::move(typeParams)) {}

  // Constructor for specialized generic class
  ClassType(std::string mangledName, std::string baseName,
            std::vector<TypePtr> typeArgs)
      : name(std::move(mangledName)),
        typeArguments(std::move(typeArgs)),
        baseGenericName(std::move(baseName)) {}

  Kind getKind() const override { return Kind::Class; }
  const std::string& getName() const { return name; }

  // Base name accessors (user-written name for error messages)
  const std::string& getBaseName() const {
    return baseName_.empty() ? name : baseName_;
  }
  void setBaseName(std::string bn) { baseName_ = std::move(bn); }
  bool hasBaseName() const { return !baseName_.empty(); }

  // Get user-friendly display name for error messages
  // For specialized classes: "Vec<i32>" or "sun.Vec<i32>"
  // For non-specialized: baseName or name with underscores converted to dots
  std::string getDisplayName() const {
    std::string base;
    if (!baseName_.empty()) {
      base = baseName_;
    } else if (!baseGenericName.empty()) {
      // Convert mangled name to dot notation: sun_Vec -> sun.Vec
      base = baseGenericName;
      for (size_t i = 0; i < base.size(); ++i) {
        if (base[i] == '_') base[i] = '.';
      }
    } else {
      base = name;
      for (size_t i = 0; i < base.size(); ++i) {
        if (base[i] == '_') base[i] = '.';
      }
    }
    if (isSpecialized() && !typeArguments.empty()) {
      std::string result = base + "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
      result += ">";
      return result;
    }
    return base;
  }

  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  const std::vector<TypePtr>& getTypeArguments() const { return typeArguments; }
  const std::string& getBaseGenericName() const { return baseGenericName; }
  bool isGenericDefinition() const { return !typeParameters.empty(); }
  bool isSpecialized() const { return !typeArguments.empty(); }
  const std::vector<ClassField>& getFields() const { return fields; }
  const std::vector<ClassMethod>& getMethods() const { return methods; }
  const std::vector<std::string>& getImplementedInterfaces() const {
    return implementedInterfaces;
  }

  bool hasField(const std::string& fieldName) const {
    return getField(fieldName) != nullptr;
  }

  void addField(const std::string& fieldName, TypePtr fieldType) {
    // Caller should check hasField() first and report error with position
    fields.push_back({fieldName, std::move(fieldType), fields.size()});
  }

  void addMethod(const std::string& methodName, TypePtr returnType,
                 std::vector<TypePtr> paramTypes, bool isConstructor = false,
                 std::vector<std::string> typeParams = {}) {
    methods.push_back({methodName, std::move(typeParams), std::move(returnType),
                       std::move(paramTypes), isConstructor});
  }

  void addImplementedInterface(const std::string& interfaceName) {
    implementedInterfaces.push_back(interfaceName);
  }

  bool implementsInterface(const std::string& interfaceName) const {
    for (const auto& iface : implementedInterfaces) {
      if (iface == interfaceName) return true;
    }
    return false;
  }

  const ClassField* getField(const std::string& fieldName) const {
    for (const auto& field : fields) {
      if (field.name == fieldName) return &field;
    }
    return nullptr;
  }

  const ClassMethod* getMethod(const std::string& methodName) const {
    for (const auto& method : methods) {
      if (method.name == methodName) return &method;
    }
    return nullptr;
  }

  // Get the constructor method (named "init")
  const ClassMethod* getConstructor() const {
    for (const auto& method : methods) {
      if (method.isConstructor) return &method;
    }
    return nullptr;
  }

  std::string toString() const override {
    if (isSpecialized() && !baseGenericName.empty()) {
      // Show specialized type like "List<i32>"
      std::string result = baseGenericName + "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
      result += ">";
      return result;
    }
    if (isGenericDefinition()) {
      // Show generic definition like "List<T>"
      std::string result = name + "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
      }
      result += ">";
      return result;
    }
    return name;
  }

  bool equals(const Type& other) const override {
    if (auto* c = dynamic_cast<const ClassType*>(&other)) {
      // For specialized types, compare by mangled name
      return name == c->name;
    }
    return false;
  }

  // Classes are value types represented as LLVM structs
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return getStructType(ctx);
  }

  // Get the actual struct type for the class
  llvm::StructType* getStructType(llvm::LLVMContext& ctx) const {
    if (cachedLLVMType) return cachedLLVMType;

    std::vector<llvm::Type*> fieldTypes;
    for (const auto& field : fields) {
      // For class-typed fields, embed the struct directly (not a pointer)
      if (field.type->isClass()) {
        auto* classType = static_cast<const ClassType*>(field.type.get());
        fieldTypes.push_back(classType->getStructType(ctx));
      } else {
        fieldTypes.push_back(field.type->toLLVMType(ctx));
      }
    }
    cachedLLVMType =
        llvm::StructType::create(ctx, fieldTypes, name + "_struct");
    return cachedLLVMType;
  }

  // Get mangled method name: ClassName_methodName
  // Class name already includes module path and library hash
  std::string getMangledMethodName(const std::string& methodName) const {
    return name + "_" + methodName;
  }
};

// Interface field information
struct InterfaceField {
  std::string name;
  TypePtr type;
};

// Interface method information
struct InterfaceMethod {
  std::string name;
  std::vector<std::string> typeParameters;  // Generic type params: <T, U>
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;  // Excludes implicit 'this' parameter
  bool hasDefaultImpl;  // true if this method has a default implementation

  bool isGeneric() const { return !typeParameters.empty(); }
};

// Forward declaration for InterfaceType
class InterfaceType;
using InterfaceTypePtr = std::shared_ptr<InterfaceType>;

// Interface type for user-defined interfaces
// Interfaces define a contract that classes must implement
class InterfaceType : public Type {
  std::string name;       // Fully qualified name (includes library hash)
  std::string baseName_;  // User-written base name for error messages
  std::vector<std::string> typeParameters;  // Generic type params: T, U, etc.
  std::vector<TypePtr>
      typeArguments;  // Type args: [i32] for specialized interfaces
  std::string
      baseGenericName;  // For specialized: original generic interface name
  std::vector<InterfaceField> fields;
  std::vector<InterfaceMethod> methods;

 public:
  InterfaceType(std::string interfaceName) : name(std::move(interfaceName)) {}

  // Constructor for generic interface definition
  InterfaceType(std::string interfaceName, std::vector<std::string> typeParams)
      : name(std::move(interfaceName)), typeParameters(std::move(typeParams)) {}

  // Constructor for specialized generic interface
  InterfaceType(std::string mangledName, std::string baseName,
                std::vector<TypePtr> typeArgs)
      : name(std::move(mangledName)),
        typeArguments(std::move(typeArgs)),
        baseGenericName(std::move(baseName)) {}

  Kind getKind() const override { return Kind::Interface; }
  const std::string& getName() const { return name; }

  // Base name accessors (user-written name for error messages)
  const std::string& getBaseName() const {
    return baseName_.empty() ? name : baseName_;
  }
  void setBaseName(std::string bn) { baseName_ = std::move(bn); }
  bool hasBaseName() const { return !baseName_.empty(); }

  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  const std::vector<TypePtr>& getTypeArguments() const { return typeArguments; }
  const std::string& getBaseGenericName() const { return baseGenericName; }
  bool isGenericDefinition() const { return !typeParameters.empty(); }
  bool isSpecialized() const { return !typeArguments.empty(); }
  const std::vector<InterfaceField>& getFields() const { return fields; }
  const std::vector<InterfaceMethod>& getMethods() const { return methods; }

  void addField(const std::string& fieldName, TypePtr fieldType) {
    // Check if field already exists
    for (const auto& existingField : fields) {
      if (existingField.name == fieldName) {
        return;
      }
    }
    fields.push_back({fieldName, std::move(fieldType)});
  }

  void addMethod(const std::string& methodName, TypePtr returnType,
                 std::vector<TypePtr> paramTypes, bool hasDefaultImpl = false,
                 std::vector<std::string> typeParams = {}) {
    methods.push_back({methodName, std::move(typeParams), std::move(returnType),
                       std::move(paramTypes), hasDefaultImpl});
  }

  const InterfaceField* getField(const std::string& fieldName) const {
    for (const auto& field : fields) {
      if (field.name == fieldName) return &field;
    }
    return nullptr;
  }

  const InterfaceMethod* getMethod(const std::string& methodName) const {
    for (const auto& method : methods) {
      if (method.name == methodName) return &method;
    }
    return nullptr;
  }

  // Get methods that don't have default implementations (must be implemented by
  // class)
  std::vector<const InterfaceMethod*> getRequiredMethods() const {
    std::vector<const InterfaceMethod*> required;
    for (const auto& method : methods) {
      if (!method.hasDefaultImpl) {
        required.push_back(&method);
      }
    }
    return required;
  }

  std::string toString() const override {
    if (isSpecialized()) {
      // Show as BaseInterface<Arg1, Arg2>
      std::string result = baseGenericName + "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
      result += ">";
      return result;
    }
    if (isGenericDefinition()) {
      // Show as Interface<T, U>
      std::string result = name + "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
      }
      result += ">";
      return result;
    }
    return name;
  }

  bool equals(const Type& other) const override {
    if (auto* i = dynamic_cast<const InterfaceType*>(&other)) {
      return name == i->name;
    }
    return false;
  }

  // Interfaces are represented as fat pointers: { ptr data, ptr vtable }
  // This enables dynamic dispatch by storing both the object pointer
  // and a pointer to the implementing class's vtable.
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return getFatPointerType(ctx);
  }

  // Get mangled method name for default implementation:
  // InterfaceName_default_methodName (name already includes library hash)
  std::string getMangledDefaultMethodName(const std::string& methodName) const {
    return name + "_default_" + methodName;
  }

  // ===================================================================
  // Dynamic Dispatch Support (vtable-based polymorphism)
  // ===================================================================

  // Get the fat pointer struct type for interface values: { ptr data, ptr
  // vtable }
  // - data: pointer to the concrete class instance
  // - vtable: pointer to the implementing class's vtable for this interface
  static llvm::StructType* getFatPointerType(llvm::LLVMContext& ctx) {
    // Check for existing named type to avoid duplicates
    if (auto* existing = llvm::StructType::getTypeByName(
            ctx, sun::StructNames::InterfaceFat)) {
      return existing;
    }
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    return llvm::StructType::create(ctx, {ptrTy, ptrTy},
                                    sun::StructNames::InterfaceFat);
  }

  // Get the vtable struct type for this interface.
  // Contains one function pointer per method in declaration order.
  // Vtable layout: [method0_ptr, method1_ptr, ...]
  llvm::StructType* getVtableType(llvm::LLVMContext& ctx) const {
    std::string vtableName = name + "_vtable_type";
    // Check for existing named type
    if (auto* existing = llvm::StructType::getTypeByName(ctx, vtableName)) {
      return existing;
    }
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    std::vector<llvm::Type*> slotTypes(methods.size(), ptrTy);
    return llvm::StructType::create(ctx, slotTypes, vtableName);
  }

  // Get the slot index for a method in the vtable.
  // Returns -1 if method not found or if the method is generic.
  // Only non-generic methods can be dispatched via vtable.
  int getMethodIndex(const std::string& methodName) const {
    int index = 0;
    for (const auto& method : methods) {
      if (method.isGeneric()) {
        continue;  // Skip generic methods - they're not in the vtable
      }
      if (method.name == methodName) {
        return index;
      }
      ++index;
    }
    return -1;  // Method not found or is generic
  }
};

// Enum variant information
struct EnumVariant {
  std::string name;
  int64_t value;  // Numeric value of the variant
};

// Forward declaration for EnumType
class EnumType;
using EnumTypePtr = std::shared_ptr<EnumType>;

// Enum type for user-defined enums
// Enums are represented as i32 values, with variants as named constants
// Example: enum Color { Red, Green, Blue }
class EnumType : public Type {
  std::string name;
  std::vector<EnumVariant> variants;

 public:
  EnumType(std::string enumName) : name(std::move(enumName)) {}

  EnumType(std::string enumName, std::vector<EnumVariant> vars)
      : name(std::move(enumName)), variants(std::move(vars)) {}

  Kind getKind() const override { return Kind::Enum; }
  const std::string& getName() const { return name; }
  const std::vector<EnumVariant>& getVariants() const { return variants; }

  void addVariant(const std::string& variantName, int64_t value) {
    variants.push_back({variantName, value});
  }

  const EnumVariant* getVariant(const std::string& variantName) const {
    for (const auto& variant : variants) {
      if (variant.name == variantName) return &variant;
    }
    return nullptr;
  }

  // Check if a variant exists by name
  bool hasVariant(const std::string& variantName) const {
    return getVariant(variantName) != nullptr;
  }

  // Get the number of variants
  size_t getNumVariants() const { return variants.size(); }

  std::string toString() const override { return name; }

  bool equals(const Type& other) const override {
    if (auto* e = dynamic_cast<const EnumType*>(&other)) {
      return name == e->name;
    }
    return false;
  }

  // Enums are represented as i32 values
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::Type::getInt32Ty(ctx);
  }

  // Get the mangled variant name: EnumName_VariantName
  std::string getMangledVariantName(const std::string& variantName) const {
    return name + "_" + variantName;
  }
};

// Type factory for common types (singleton pattern)
class Types {
 public:
  static TypePtr Void() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Void);
    return t;
  }
  static TypePtr Bool() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Bool);
    return t;
  }
  static TypePtr Int8() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Int8);
    return t;
  }
  static TypePtr Int16() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Int16);
    return t;
  }
  static TypePtr Int32() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Int32);
    return t;
  }
  static TypePtr Int64() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Int64);
    return t;
  }
  static TypePtr UInt8() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::UInt8);
    return t;
  }
  static TypePtr UInt16() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::UInt16);
    return t;
  }
  static TypePtr UInt32() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::UInt32);
    return t;
  }
  static TypePtr UInt64() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::UInt64);
    return t;
  }
  static TypePtr Float32() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Float32);
    return t;
  }
  static TypePtr Float64() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Float64);
    return t;
  }

  // Slice type singleton: builtin { i64 start, i64 end } for indexing
  static TypePtr Slice() {
    static auto t = std::make_shared<SliceType>();
    return t;
  }

  // Module reference type for qualified name resolution (mod_x.mod_y.var)
  static TypePtr Module(const std::string& modulePath) {
    return std::make_shared<ModuleType>(modulePath);
  }

  // String literals are represented as static_ptr<u8> - immortal, read-only
  // data
  static TypePtr String() { return StaticPointer(UInt8()); }

  // Create a function type: _() -> {} (named function, direct call)
  static TypePtr Function(TypePtr returnType, std::vector<TypePtr> paramTypes) {
    return std::make_shared<FunctionType>(std::move(returnType),
                                          std::move(paramTypes));
  }

  // Create a lambda type: () -> {} (anonymous function, fat pointer call)
  static TypePtr Lambda(TypePtr returnType, std::vector<TypePtr> paramTypes) {
    return std::make_shared<LambdaType>(std::move(returnType),
                                        std::move(paramTypes));
  }

  // Create a raw (non-owning) pointer type: raw_ptr<T> for C interop
  static TypePtr RawPointer(TypePtr pointeeType) {
    return std::make_shared<RawPointerType>(std::move(pointeeType));
  }

  // Create a static pointer type: static_ptr<T> for immortal data
  // Used for string literals and global constants - memory safe
  static TypePtr StaticPointer(TypePtr pointeeType) {
    return std::make_shared<StaticPointerType>(std::move(pointeeType));
  }

  // Create a null pointer type (singleton)
  static TypePtr NullPointer() {
    static auto t = std::make_shared<NullPointerType>();
    return t;
  }

  // Create a reference type: ref(T) with implicit dereferencing
  static TypePtr Reference(TypePtr referencedType, bool isMutable = true) {
    return std::make_shared<ReferenceType>(std::move(referencedType),
                                           isMutable);
  }

  // Create a fixed-size array type: array<T, N> or array<T, M, N>
  static TypePtr Array(TypePtr elementType, std::vector<size_t> dimensions) {
    return std::make_shared<ArrayType>(std::move(elementType),
                                       std::move(dimensions));
  }

  // Create a class type (cached by name)
  static std::shared_ptr<ClassType> Class(const std::string& name) {
    auto& cache = getClassCache();
    auto it = cache.find(name);
    if (it != cache.end()) {
      return it->second;
    }
    auto type = std::make_shared<ClassType>(name);
    cache[name] = type;
    return type;
  }

  // Create a generic class type with type parameters
  static std::shared_ptr<ClassType> GenericClass(
      const std::string& name, std::vector<std::string> typeParams) {
    std::string key = name + "<";
    for (size_t i = 0; i < typeParams.size(); ++i) {
      if (i > 0) key += ",";
      key += typeParams[i];
    }
    key += ">";

    auto& cache = getGenericClassCache();
    auto it = cache.find(key);
    if (it != cache.end()) {
      return it->second;
    }
    auto type = std::make_shared<ClassType>(name, std::move(typeParams));
    cache[key] = type;
    return type;
  }

  // Create a specialized generic class (e.g., List<i32>)
  static std::shared_ptr<ClassType> SpecializedClass(
      const std::string& baseName, std::vector<TypePtr> typeArgs) {
    std::string mangledName = mangleGenericClassName(baseName, typeArgs);

    auto& cache = getSpecializedClassCache();
    auto it = cache.find(mangledName);
    if (it != cache.end()) {
      return it->second;
    }
    auto type =
        std::make_shared<ClassType>(mangledName, baseName, std::move(typeArgs));
    cache[mangledName] = type;
    return type;
  }

  // Generate mangled name for a specialized generic class
  static std::string mangleGenericClassName(
      const std::string& baseName, const std::vector<TypePtr>& typeArgs) {
    std::string result = baseName;
    for (const auto& arg : typeArgs) {
      result += "_" + mangleTypeName(arg);
    }
    return result;
  }

  // Generate mangled name for a type (for use in function names, etc.)
  static std::string mangleTypeName(const TypePtr& type) {
    if (!type) return "void";
    if (type->isPrimitive()) {
      auto* prim = dynamic_cast<const PrimitiveType*>(type.get());
      return prim->toString();
    }
    if (type->isClass()) {
      auto* cls = dynamic_cast<const ClassType*>(type.get());
      return cls->getName();  // Already mangled if specialized
    }
    if (type->isTypeParameter()) {
      auto* tp = dynamic_cast<const TypeParameterType*>(type.get());
      return tp->getName();
    }
    return type->toString();
  }

  // Create a type parameter type
  static TypePtr TypeParameter(const std::string& name) {
    return std::make_shared<TypeParameterType>(name);
  }

  // Create an interface type (cached by name)
  static std::shared_ptr<InterfaceType> Interface(const std::string& name) {
    auto& cache = getInterfaceCache();
    auto it = cache.find(name);
    if (it != cache.end()) {
      return it->second;
    }
    auto type = std::make_shared<InterfaceType>(name);
    cache[name] = type;
    return type;
  }

  // Clear all type caches - must be called between independent compilation
  // units (e.g., between tests)
  static void clearCaches() {
    getClassCache().clear();
    getGenericClassCache().clear();
    getSpecializedClassCache().clear();
    getInterfaceCache().clear();
  }

 private:
  // Cache accessors for clearCaches() support
  static std::unordered_map<std::string, std::shared_ptr<ClassType>>&
  getClassCache() {
    static std::unordered_map<std::string, std::shared_ptr<ClassType>> cache;
    return cache;
  }
  static std::unordered_map<std::string, std::shared_ptr<ClassType>>&
  getGenericClassCache() {
    static std::unordered_map<std::string, std::shared_ptr<ClassType>> cache;
    return cache;
  }
  static std::unordered_map<std::string, std::shared_ptr<ClassType>>&
  getSpecializedClassCache() {
    static std::unordered_map<std::string, std::shared_ptr<ClassType>> cache;
    return cache;
  }
  static std::unordered_map<std::string, std::shared_ptr<InterfaceType>>&
  getInterfaceCache() {
    static std::unordered_map<std::string, std::shared_ptr<InterfaceType>>
        cache;
    return cache;
  }

 public:
  // Parse a type name string to TypePtr
  static TypePtr fromString(const std::string& name) {
    if (name == "void") return Void();
    if (name == "bool") return Bool();
    if (name == "i8") return Int8();
    if (name == "i16") return Int16();
    if (name == "i32") return Int32();
    if (name == "i64") return Int64();
    if (name == "u8") return UInt8();
    if (name == "u16") return UInt16();
    if (name == "u32") return UInt32();
    if (name == "u64") return UInt64();
    if (name == "f32") return Float32();
    if (name == "f64") return Float64();
    if (name == "slice") return Slice();
    return nullptr;  // Unknown type
  }
};

/**
 * TypeRegistry - Per-compilation-unit registry for class and interface types.
 *
 * This replaces the static caches in Types class to avoid cross-test pollution.
 * Create one TypeRegistry per compilation and share it between SemanticAnalyzer
 * and CodegenVisitor.
 */
class TypeRegistry {
  std::unordered_map<std::string, std::shared_ptr<ClassType>> classCache;
  std::unordered_map<std::string, std::shared_ptr<ClassType>> genericClassCache;
  std::unordered_map<std::string, std::shared_ptr<ClassType>>
      specializedClassCache;
  std::unordered_map<std::string, std::shared_ptr<InterfaceType>>
      interfaceCache;
  std::unordered_map<std::string, std::shared_ptr<InterfaceType>>
      genericInterfaceCache;
  std::unordered_map<std::string, std::shared_ptr<InterfaceType>>
      specializedInterfaceCache;
  std::unordered_map<std::string, std::shared_ptr<EnumType>> enumCache;

 public:
  TypeRegistry() { registerBuiltins(); }

  // Register built-in types like IError, IIterator<T>, IIterable<T>
  void registerBuiltins() {
    // Create IError interface with code() and message() methods
    auto ierror = std::make_shared<InterfaceType>("IError");
    ierror->addMethod("code", Types::Int32(), {}, true);  // code() -> i32
    ierror->addMethod("message", Types::String(), {},
                      true);  // message() -> static_ptr<u8>
    interfaceCache["IError"] = ierror;

    // Create IIterator<T, Container> generic interface
    // Methods: hasNext(ref Container) -> bool, next(ref Container) -> T
    auto iiterator = std::make_shared<InterfaceType>(
        "IIterator", std::vector<std::string>{"T", "Container"});
    iiterator->addMethod("hasNext", Types::Bool(),
                         {Types::Reference(Types::TypeParameter("Container"))},
                         false);
    iiterator->addMethod("next", Types::TypeParameter("T"),
                         {Types::Reference(Types::TypeParameter("Container"))},
                         false);
    genericInterfaceCache["IIterator<T,Container>"] = iiterator;
    // Also register by base name for lookup
    interfaceCache["IIterator"] = iiterator;

    // Create IIterable<T, Self> generic interface
    // Methods: iter() -> IIterator<T, Self>
    // Self is the type of the container implementing IIterable
    auto iiterable = std::make_shared<InterfaceType>(
        "IIterable", std::vector<std::string>{"T", "Self"});
    // Create IIterator<T, Self> return type
    auto iteratorReturnType = std::make_shared<InterfaceType>(
        "IIterator", "IIterator",
        std::vector<TypePtr>{Types::TypeParameter("T"),
                             Types::TypeParameter("Self")});
    iiterable->addMethod("iter", iteratorReturnType, {}, false);
    genericInterfaceCache["IIterable<T,Self>"] = iiterable;
    // Also register by base name for lookup
    interfaceCache["IIterable"] = iiterable;
  }

  // Check if a type name is a builtin type that cannot be redefined
  // Includes builtin interfaces and type traits used by _is<T>
  bool isBuiltinTypeName(const std::string& name) const {
    static const std::unordered_set<std::string> builtinNames = {
        // Builtin interfaces
        "IError", "IIterator", "IIterable",
        // Type traits for _is<T> intrinsic
        "_Integer", "_Signed", "_Unsigned", "_Float", "_Numeric", "_Primitive"};
    return builtinNames.count(name) > 0;
  }

  // Non-copyable to prevent accidental duplication
  TypeRegistry(const TypeRegistry&) = delete;
  TypeRegistry& operator=(const TypeRegistry&) = delete;

  // Movable
  TypeRegistry(TypeRegistry&&) = default;
  TypeRegistry& operator=(TypeRegistry&&) = default;

  // Get or create a class type by name
  // Also checks specializedClassCache for generic instantiations
  std::shared_ptr<ClassType> getClass(const std::string& name) {
    // First check specialized class cache (for generic instantiations like
    // Box_i32)
    auto specIt = specializedClassCache.find(name);
    if (specIt != specializedClassCache.end()) {
      return specIt->second;
    }

    // Then check regular class cache
    auto it = classCache.find(name);
    if (it != classCache.end()) {
      return it->second;
    }
    auto type = std::make_shared<ClassType>(name);
    classCache[name] = type;
    return type;
  }

  // Get or create a generic class type with type parameters
  std::shared_ptr<ClassType> getGenericClass(
      const std::string& name, std::vector<std::string> typeParams) {
    std::string key = name + "<";
    for (size_t i = 0; i < typeParams.size(); ++i) {
      if (i > 0) key += ",";
      key += typeParams[i];
    }
    key += ">";

    auto it = genericClassCache.find(key);
    if (it != genericClassCache.end()) {
      return it->second;
    }
    auto type = std::make_shared<ClassType>(name, std::move(typeParams));
    genericClassCache[key] = type;
    return type;
  }

  // Get or create a specialized generic class (e.g., List<i32>)
  std::shared_ptr<ClassType> getSpecializedClass(
      const std::string& baseName, std::vector<TypePtr> typeArgs) {
    std::string mangledName = Types::mangleGenericClassName(baseName, typeArgs);

    auto it = specializedClassCache.find(mangledName);
    if (it != specializedClassCache.end()) {
      return it->second;
    }
    auto type =
        std::make_shared<ClassType>(mangledName, baseName, std::move(typeArgs));
    specializedClassCache[mangledName] = type;
    return type;
  }

  // Get or create an interface type by name
  // Also checks specializedInterfaceCache for generic instantiations
  std::shared_ptr<InterfaceType> getInterface(const std::string& name) {
    // First check specialized interface cache
    auto specIt = specializedInterfaceCache.find(name);
    if (specIt != specializedInterfaceCache.end()) {
      return specIt->second;
    }

    // Then check regular interface cache
    auto it = interfaceCache.find(name);
    if (it != interfaceCache.end()) {
      return it->second;
    }
    auto type = std::make_shared<InterfaceType>(name);
    interfaceCache[name] = type;
    return type;
  }

  // Look up an interface by name without auto-creating
  // Returns nullptr if not found
  std::shared_ptr<InterfaceType> lookupInterface(
      const std::string& name) const {
    // Check specialized interface cache first
    auto specIt = specializedInterfaceCache.find(name);
    if (specIt != specializedInterfaceCache.end()) {
      return specIt->second;
    }
    // Then check regular interface cache
    auto it = interfaceCache.find(name);
    if (it != interfaceCache.end()) {
      return it->second;
    }
    return nullptr;
  }

  // Get or create a generic interface type with type parameters
  std::shared_ptr<InterfaceType> getGenericInterface(
      const std::string& name, std::vector<std::string> typeParams) {
    std::string key = name + "<";
    for (size_t i = 0; i < typeParams.size(); ++i) {
      if (i > 0) key += ",";
      key += typeParams[i];
    }
    key += ">";

    auto it = genericInterfaceCache.find(key);
    if (it != genericInterfaceCache.end()) {
      return it->second;
    }
    auto type = std::make_shared<InterfaceType>(name, std::move(typeParams));
    genericInterfaceCache[key] = type;
    return type;
  }

  // Get or create a specialized generic interface (e.g., IIterator<i32>)
  std::shared_ptr<InterfaceType> getSpecializedInterface(
      const std::string& baseName, std::vector<TypePtr> typeArgs) {
    std::string mangledName = Types::mangleGenericClassName(baseName, typeArgs);

    auto it = specializedInterfaceCache.find(mangledName);
    if (it != specializedInterfaceCache.end()) {
      return it->second;
    }
    auto type = std::make_shared<InterfaceType>(mangledName, baseName,
                                                std::move(typeArgs));
    specializedInterfaceCache[mangledName] = type;
    return type;
  }

  // Get or create an enum type by name
  std::shared_ptr<EnumType> getEnum(const std::string& name) {
    auto it = enumCache.find(name);
    if (it != enumCache.end()) {
      return it->second;
    }
    auto type = std::make_shared<EnumType>(name);
    enumCache[name] = type;
    return type;
  }

  // Check if an enum type exists
  bool hasEnum(const std::string& name) const {
    return enumCache.find(name) != enumCache.end();
  }

  // Clear all caches (useful for REPL reset)
  void clear() {
    classCache.clear();
    genericClassCache.clear();
    specializedClassCache.clear();
    interfaceCache.clear();
    genericInterfaceCache.clear();
    specializedInterfaceCache.clear();
    enumCache.clear();
  }
};

// Inline implementations for Type methods
inline bool Type::isNumeric() const {
  Kind k = getKind();
  return k == Kind::Int8 || k == Kind::Int16 || k == Kind::Int32 ||
         k == Kind::Int64 || k == Kind::UInt8 || k == Kind::UInt16 ||
         k == Kind::UInt32 || k == Kind::UInt64 || k == Kind::Float32 ||
         k == Kind::Float64;
}

inline bool Type::isIntegral() const {
  Kind k = getKind();
  return k == Kind::Int8 || k == Kind::Int16 || k == Kind::Int32 ||
         k == Kind::Int64 || k == Kind::UInt8 || k == Kind::UInt16 ||
         k == Kind::UInt32 || k == Kind::UInt64;
}

inline bool Type::isFloatingPoint() const {
  Kind k = getKind();
  return k == Kind::Float32 || k == Kind::Float64;
}

inline bool Type::isString() const {
  // String is now represented as static_ptr<u8> - immortal string literal data
  if (auto* p = dynamic_cast<const StaticPointerType*>(this)) {
    return p->getPointeeType()->isUInt8();
  }
  return false;
}

// Out-of-line definition for RawPointerType::equals (needs StaticPointerType
// complete)
inline bool RawPointerType::equals(const Type& other) const {
  // Raw pointer is compatible with null
  if (other.isNullPointer()) return true;
  // Raw pointer is compatible with static_ptr (static_ptr can convert to
  // raw_ptr)
  if (auto* s = dynamic_cast<const StaticPointerType*>(&other)) {
    return pointeeType->equals(*s->getPointeeType());
  }
  if (auto* p = dynamic_cast<const RawPointerType*>(&other)) {
    return pointeeType->equals(*p->pointeeType);
  }
  return false;
}

}  // namespace sun
