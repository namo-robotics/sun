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

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/struct_names.h"
#include "semantic_analysis/visibility.h"
#include "support/error.h"

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
    Char,     // char: one Unicode scalar value, 4 bytes
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
    Module,         // Module/namespace reference (for mod_x.mod_y.var access)
  };

  virtual ~Type() = default;
  virtual Kind getKind() const = 0;
  virtual std::string toString() const = 0;
  // User-friendly name for error messages (strips internal prefixes)
  virtual std::string toDisplayString() const { return toString(); }
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
  bool isChar() const { return getKind() == Kind::Char; }
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
  // `char` is primitive (passed by value, no ownership) but deliberately not
  // numeric or integral: arithmetic and implicit widening are keyed off
  // isNumeric()/isIntegral(), so leaving it out of those rejects `'a' + 1`
  // and any silent char/integer mixing.
  bool isPrimitive() const {
    Kind k = getKind();
    return k == Kind::Void || k == Kind::Bool || k == Kind::Int8 ||
           k == Kind::Int16 || k == Kind::Int32 || k == Kind::Int64 ||
           k == Kind::UInt8 || k == Kind::UInt16 || k == Kind::UInt32 ||
           k == Kind::UInt64 || k == Kind::Float32 || k == Kind::Float64 ||
           k == Kind::Char;
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
  // Compound types must be passed by reference (classes, interfaces, arrays,
  // payload-carrying enums). Payload-free enums are NOT compound - they are
  // i32 values and passed by value. Defined out-of-line (needs EnumType).
  bool isCompound() const;
  bool isNumeric() const;
  bool isIntegral() const;
  bool isFloatingPoint() const;
  bool isString() const;
};

// Something computed from a type parameter rather than the parameter itself.
// `_return_type_of<F>` names a type that is only known once F is, so until
// then it travels as the parameter F plus the projection to apply to it.
enum class TypeProjection : uint8_t {
  None,        // the parameter itself
  ReturnType,  // _return_type_of<F>: what F returns
};

// Type parameter (used in generic class/function definitions)
// Represents a type variable like T, U, V in class List<T>
class TypeParameterType : public Type {
  std::string name;   // Parameter name: T, U, etc. (F$ret when projected)
  std::string base_;  // The parameter the projection applies to
  TypeProjection projection_ = TypeProjection::None;
  // What `<T: Trait>` promised about whatever T stands for. Metadata only —
  // intentionally excluded from equals()/toString() so it never disturbs
  // substitution or identity. It travels with the parameter so a body being
  // analyzed with T still standing for itself can see what T is known to be.
  std::string constraint_;

 public:
  explicit TypeParameterType(std::string paramName, std::string constraint = "",
                             TypeProjection projection = TypeProjection::None)
      : name(paramName),
        base_(std::move(paramName)),
        projection_(projection),
        constraint_(std::move(constraint)) {
    // A projection needs a name of its own, or `Thread<F>` and
    // `Thread<_return_type_of<F>>` would mangle to one specialization.
    if (projection_ == TypeProjection::ReturnType) name = base_ + "$ret";
  }

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::TypeParameter;
  Kind getKind() const override { return StaticKind; }
  const std::string& getName() const { return name; }
  const std::string& getProjectionBase() const { return base_; }
  TypeProjection getProjection() const { return projection_; }
  const std::string& getConstraint() const { return constraint_; }
  bool hasConstraint() const { return !constraint_.empty(); }

  std::string toString() const override { return name; }

  std::string toDisplayString() const override {
    return projection_ == TypeProjection::ReturnType
               ? "_return_type_of<" + base_ + ">"
               : name;
  }

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
      case Kind::Char:
        return "char";
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
      case Kind::Char:
        return llvm::Type::getInt32Ty(ctx);  // a Unicode scalar value
      default:
        return nullptr;
    }
  }
};

// A non-null, one-word pointer to a module-scope function.
// Type annotation: function (Args) Result
class FunctionType : public Type {
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;
  bool canThrow_ = false;  // declared with 'throws IError' — may unwind

 public:
  FunctionType(TypePtr ret, std::vector<TypePtr> params, bool canThrow = false)
      : returnType(std::move(ret)),
        paramTypes(std::move(params)),
        canThrow_(canThrow) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Function;
  Kind getKind() const override { return StaticKind; }
  const TypePtr& getReturnType() const { return returnType; }
  const std::vector<TypePtr>& getParamTypes() const { return paramTypes; }

  // Whether calls through this pointer may throw.
  bool canThrow() const { return canThrow_; }
  void setCanThrow(bool v) { canThrow_ = v; }

  std::string toString() const override {
    std::string result = "function (";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toString();
    }
    result += ") " + returnType->toString();
    if (canThrow_) result += " throws IError";
    return result;
  }

  std::string toDisplayString() const override {
    std::string result = "function (";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toDisplayString();
    }
    result += ") " + returnType->toDisplayString();
    if (canThrow_) result += " throws IError";
    return result;
  }

  bool equals(const Type& other) const override {
    if (auto* f = dynamic_cast<const FunctionType*>(&other)) {
      if (canThrow_ != f->canThrow_) return false;
      if (!returnType->equals(*f->returnType)) return false;
      if (paramTypes.size() != f->paramTypes.size()) return false;
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (!paramTypes[i]->equals(*f->paramTypes[i])) return false;
      }
      return true;
    }
    return false;
  }

  // Function values use LLVM's opaque pointer representation.
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::PointerType::getUnqual(ctx);
  }

  // Returns a pointer to the function type (for function pointer variables)
  llvm::Type* toPointerType(llvm::LLVMContext& ctx) const {
    return toLLVMType(ctx);
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
// Type annotation: () => {}
class LambdaType : public Type {
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;
  bool canThrow_ = false;  // declared with 'throws IError' — may unwind
  // Part of the type's identity: `<'_>() => T` in source. True when the
  // lambda carries a captured environment that lives in a stack frame
  // (capture lists, bound methods). A plain '() => T' is reserved for
  // environment-free lambdas, so this is what keeps a frame-bound lambda
  // from laundering through an annotation-typed parameter or field.
  bool hasRefCaptures_ = false;
  // Metadata, NOT identity: the lifetime name written on this position
  // ('<'a>(i32) => i32'), empty when elided. Names only mean something
  // relative to one signature's lifetime list, so equals(), toString() and
  // mangling ignore them - '<'a>' and '<'b>' are one type.
  std::string lifetimeName_;

 public:
  LambdaType(TypePtr ret, std::vector<TypePtr> params, bool canThrow = false)
      : returnType(std::move(ret)),
        paramTypes(std::move(params)),
        canThrow_(canThrow) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Lambda;
  Kind getKind() const override { return StaticKind; }
  const TypePtr& getReturnType() const { return returnType; }
  const std::vector<TypePtr>& getParamTypes() const { return paramTypes; }
  bool canThrow() const { return canThrow_; }
  bool hasRefCaptures() const { return hasRefCaptures_; }
  void setHasRefCaptures(bool v) { hasRefCaptures_ = v; }
  const std::string& getLifetimeName() const { return lifetimeName_; }
  void setLifetimeName(std::string name) { lifetimeName_ = std::move(name); }

  std::string toString() const override {
    std::string result = hasRefCaptures_ ? "<'_>(" : "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toString();
    }
    result += ") => " + returnType->toString();
    if (canThrow_) result += " throws IError";
    return result;
  }

  std::string toDisplayString() const override {
    std::string result = hasRefCaptures_ ? "<'_>(" : "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toDisplayString();
    }
    result += ") => " + returnType->toDisplayString();
    if (canThrow_) result += " throws IError";
    return result;
  }

  bool equals(const Type& other) const override {
    if (auto* l = dynamic_cast<const LambdaType*>(&other)) {
      if (canThrow_ != l->canThrow_) return false;
      if (hasRefCaptures_ != l->hasRefCaptures_) return false;
      if (!returnType->equals(*l->returnType)) return false;
      if (paramTypes.size() != l->paramTypes.size()) return false;
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (!paramTypes[i]->equals(*l->paramTypes[i])) return false;
      }
      return true;
    }
    return false;
  }

  // Same signature ignoring throwing-ness and the lifetime marker. Used by the
  // bound-method overload chooser, which picks a method by shape before the
  // chosen value is flagged as frame-bound; assignability rejects it later.
  bool equalsIgnoringThrow(const LambdaType& other) const {
    if (!returnType->equals(*other.returnType)) return false;
    if (paramTypes.size() != other.paramTypes.size()) return false;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (!paramTypes[i]->equals(*other.paramTypes[i])) return false;
    }
    return true;
  }

  // Can a by-value 'from' argument bind to a parameter of this type?
  // Same signature, and each marker only widens: a non-throwing lambda may
  // go where a throwing one is expected, and an environment-free lambda may
  // go where a '<'_>' one is expected — never the other way around.
  bool acceptsValueOf(const LambdaType& from) const {
    if (!equalsIgnoringThrow(from)) return false;
    if (!canThrow_ && from.canThrow_) return false;
    if (!hasRefCaptures_ && from.hasRefCaptures_) return false;
    return true;
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

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::RawPointer;
  Kind getKind() const override { return StaticKind; }
  const TypePtr& getPointeeType() const { return pointeeType; }

  std::string toString() const override {
    return "raw_ptr(" + pointeeType->toString() + ")";
  }

  std::string toDisplayString() const override {
    return "raw_ptr<" + pointeeType->toDisplayString() + ">";
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

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::StaticPointer;
  Kind getKind() const override { return StaticKind; }
  const TypePtr& getPointeeType() const { return pointeeType; }

  std::string toString() const override {
    return "static_ptr(" + pointeeType->toString() + ")";
  }

  std::string toDisplayString() const override {
    return "static_ptr<" + pointeeType->toDisplayString() + ">";
  }

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

  // Returns fat pointer struct: { ptr data, i64 length }
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    if (!cachedLLVMType) {
      // Check if the type already exists in the context (avoids creating
      // duplicates like static_ptr_struct.0, static_ptr_struct.1, etc.)
      cachedLLVMType =
          llvm::StructType::getTypeByName(ctx, sun::StructNames::StaticPtr);
      if (!cachedLLVMType) {
        cachedLLVMType = llvm::StructType::create(
            ctx,
            {llvm::PointerType::getUnqual(ctx), llvm::Type::getInt64Ty(ctx)},
            sun::StructNames::StaticPtr);
      }
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

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::NullPointer;
  Kind getKind() const override { return StaticKind; }

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
  // Metadata, NOT identity: the lifetime name written on this position
  // ('ref 'a Bus'), empty when elided. Ignored by equals() and toString(),
  // exactly like LambdaType's - see the comment there.
  std::string lifetimeName_;
  std::vector<std::string> classLifetimeArgs_;

 public:
  explicit ReferenceType(TypePtr referenced, bool isMutable = true)
      : referencedType(std::move(referenced)), mutable_(isMutable) {}

  const std::string& getLifetimeName() const { return lifetimeName_; }
  void setLifetimeName(std::string name) { lifetimeName_ = std::move(name); }
  // Lifetime arguments applied to the referent's class ('ref Bus<'this>'):
  // positionally binding the class's declared lifetimes. Metadata like
  // lifetimeName_ - never part of the type's identity.
  const std::vector<std::string>& getClassLifetimeArgs() const {
    return classLifetimeArgs_;
  }
  void setClassLifetimeArgs(std::vector<std::string> args) {
    classLifetimeArgs_ = std::move(args);
  }

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Reference;
  Kind getKind() const override { return StaticKind; }
  const TypePtr& getReferencedType() const { return referencedType; }
  bool isMutable() const { return mutable_; }

  // Check if this is a reference to an unsized array
  // Implemented after ArrayType definition
  inline bool isUnsizedArrayRef() const;

  std::string toString() const override {
    return std::string(mutable_ ? "ref(" : "const ref(") +
           referencedType->toString() + ")";
  }

  std::string toDisplayString() const override {
    return std::string(mutable_ ? "ref " : "const ref ") +
           referencedType->toDisplayString();
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

// "i32, ref Vec<i32>" — a list of types as it reads in a diagnostic.
inline std::string formatTypeList(const std::vector<TypePtr>& types) {
  std::string out;
  for (size_t i = 0; i < types.size(); ++i) {
    if (i > 0) out += ", ";
    out += types[i] ? types[i]->toDisplayString() : "unknown";
  }
  return out;
}

// Helper: unwrap reference types (ref(T) -> T, otherwise unchanged)
// References should behave like values, transparently dereferenced
inline TypePtr unwrapRef(TypePtr type) {
  if (type && type->isReference()) {
    return static_cast<const ReferenceType*>(type.get())->getReferencedType();
  }
  return type;
}

// `ref T` (the referent may be changed through it)
inline bool isMutableRef(const TypePtr& type) {
  return type && type->isReference() &&
         static_cast<const ReferenceType*>(type.get())->isMutable();
}

// `const ref T` (the referent may only be read through it)
inline bool isConstRef(const TypePtr& type) {
  return type && type->isReference() &&
         !static_cast<const ReferenceType*>(type.get())->isMutable();
}

// A reference of kind `from` may stand in for one of kind `to` unless that
// would let a const borrow be written through
inline bool refMutabilityConvertible(const ReferenceType& from,
                                     const ReferenceType& to) {
  return from.isMutable() || !to.isMutable();
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

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::ErrorUnion;
  Kind getKind() const override { return StaticKind; }
  const TypePtr& getValueType() const { return valueType; }

  std::string toString() const override {
    return valueType->toString() + ", error";
  }

  // Spelled the way the source spells it: "i32 throws IError"
  std::string toDisplayString() const override {
    return valueType->toDisplayString() + " throws IError";
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

// Array type: array<T, N> or array<T, M, N> for multi-dimensional.
// A sized array OWNS its elements inline - [N x T] wherever it lives (a local,
// a field, a global, an element of another array) - and moves like every
// other compound value. An unsized array<T> (empty dimensions) is a view of
// some sized array with the rank erased; it exists only behind `ref` and is
// carried as the fat struct { ptr data, i32 ndims, ptr dims }.
class ArrayType : public Type {
  TypePtr elementType;             // The element type (e.g., i32)
  std::vector<size_t> dimensions;  // Fixed sizes (e.g., {5} or {3, 2}), empty
                                   // means unsized

 public:
  ArrayType(TypePtr elemType, std::vector<size_t> dims)
      : elementType(std::move(elemType)), dimensions(std::move(dims)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Array;
  Kind getKind() const override { return StaticKind; }
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

  std::string toDisplayString() const override {
    std::string result = "array<" + elementType->toDisplayString();
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

  // The view struct { ptr data, i32 ndims, ptr dims } that a `ref array<T>`
  // is carried as: the element storage, the rank, and the dimension sizes
  // (an i64 table that outlives the view - a private constant global).
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

  // A sized array is its inline storage; an unsized one is the view struct
  // (reached only through ReferenceType::toLLVMType).
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    if (isUnsized()) return getArrayStructType(ctx);
    return getDataStorageType(ctx);
  }

  // The inline storage type of a sized array: [3 x [2 x i32]] for
  // array<i32, 3, 2>. Null for an unsized array.
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

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Slice;
  Kind getKind() const override { return StaticKind; }

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

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Module;
  Kind getKind() const override { return StaticKind; }
  const std::string& getModulePath() const { return modulePath; }

  std::string toString() const override { return "module<" + modulePath + ">"; }

  // Drop the "$hash$_" a moon import prefixes and spell the path with dots
  std::string toDisplayString() const override {
    std::string path =
        modulePath.substr(QualifiedName::extractHashPrefix(modulePath).size());
    for (char& c : path) {
      if (c == '_') c = '.';
    }
    return "module<" + path + ">";
  }

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

// A reference is the referent's address, except a `ref array<T>` to an
// unsized array, which IS the view struct { ptr data, i32 ndims, ptr dims }:
// the view is born at the borrow site from a sized array and travels by value.
inline llvm::Type* ReferenceType::toLLVMType(llvm::LLVMContext& ctx) const {
  if (isUnsizedArrayRef()) return ArrayType::getArrayStructType(ctx);
  return llvm::PointerType::getUnqual(ctx);
}

// Class field information
struct ClassField {
  std::string name;
  TypePtr type;
  size_t index;  // Index in the struct
  sun::Visibility visibility = sun::Visibility::Private;
};

// Class method information
struct ClassMethod {
  std::string name;
  std::vector<std::string> typeParameters;  // Generic type params: <T, U>
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;  // Excludes implicit 'this' parameter
  bool isConstructor;               // true if this is the 'init' method
  bool canThrow = false;  // declared with 'throws IError' — may unwind
  bool isConst = false;   // `const function`: does not change `this`
  sun::Visibility visibility = sun::Visibility::Private;
  bool isSynthesizedConstructor = false;

  bool isGeneric() const { return !typeParameters.empty(); }
};

// Method info for ClassType/InterfaceType method tables.
// Lightweight alternative to FunctionInfo (no Capture dependency).
// Stores enough for overload resolution and codegen name lookup.
struct ScopeMethodInfo {
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;  // Excludes implicit 'this' parameter
  std::string
      qualifiedName;     // Mangled name for LLVM codegen (e.g., "MyClass_foo")
  std::string baseName;  // User-written method name (e.g., "foo")
};

// Indexed method table: O(1) name-based overload lookup + O(1) exact sig
// lookup. Used on ClassType and InterfaceType for method resolution.
class ScopeMethodTable {
 public:
  using iterator = std::unordered_map<std::string, ScopeMethodInfo>::iterator;
  using const_iterator =
      std::unordered_map<std::string, ScopeMethodInfo>::const_iterator;

  ScopeMethodInfo& operator[](const std::string& sig) {
    auto [it, inserted] = bySig_.emplace(sig, ScopeMethodInfo{});
    if (inserted) {
      std::string name = extractName(sig);
      byName_[name].push_back(&it->second);
    }
    return it->second;
  }

  bool contains(const std::string& sig) const { return bySig_.count(sig) > 0; }

  const_iterator find(const std::string& sig) const { return bySig_.find(sig); }

  const_iterator end() const { return bySig_.end(); }
  const_iterator begin() const { return bySig_.begin(); }
  iterator end() { return bySig_.end(); }
  iterator begin() { return bySig_.begin(); }
  bool empty() const { return bySig_.empty(); }
  size_t size() const { return bySig_.size(); }

  // Check if any method with this base name exists (O(1))
  bool hasName(const std::string& name) const {
    return byName_.count(name) > 0;
  }

  // Get all overloads for a given base name (O(1) lookup)
  const std::vector<ScopeMethodInfo*>* getOverloads(
      const std::string& name) const {
    auto it = byName_.find(name);
    if (it != byName_.end()) return &it->second;
    return nullptr;
  }

 private:
  std::unordered_map<std::string, ScopeMethodInfo> bySig_;
  std::unordered_map<std::string, std::vector<ScopeMethodInfo*>> byName_;

  // Extract method name from signature "name(type1,type2)"
  static std::string extractName(const std::string& sig) {
    auto paren = sig.find('(');
    return paren != std::string::npos ? sig.substr(0, paren) : sig;
  }
};

// Class type for user-defined classes
// Classes are represented as LLVM structs with methods as separate functions
// Generic classes have type parameters (e.g., class List<T>)
// Specialized classes have type arguments (e.g., List<i32>)
class ClassType : public Type {
  std::string mangledName;  // Fully qualified name (e.g., "$hash$_std_Vec")
  std::string
      baseName_;  // User-written base name (e.g., "Unique") for error messages
  sun::QualifiedName qualifiedName_;  // Structured qualified name for scoping
  std::vector<std::string>
      typeParameters;  // Type params: ["T", "U"] for generic definitions
  std::vector<TypePtr>
      typeArguments;            // Type args: [i32] for specialized classes
  std::string baseGenericName;  // For specialized: original generic class name
  sun::QualifiedName
      genericQualifiedName_;  // For specialized: the generic's qualified name
  std::vector<ClassField> fields;
  std::vector<ClassMethod> methods;
  ScopeMethodTable
      methodTable_;  // Indexed method table for overload resolution
  std::vector<std::string>
      implementedInterfaces;  // Names of interfaces this class implements
  std::vector<std::string>
      staticOnlyInterfaces;  // Implemented, but not convertible to (see below)
  bool isPacked_ = false;    // "packed class": lay fields out with no padding
  // Lifetime names the class DECLARES ('class Bus<'a>'). Declarations only,
  // never bindings, so sharing one ClassType per class stays sound; the
  // borrow checker uses them to entangle a method's named parameters with
  // the receiver at call sites.
  std::vector<std::string> lifetimeParams_;
  mutable llvm::StructType* cachedLLVMType = nullptr;

 public:
  sun::Visibility visibility = sun::Visibility::Private;

  const std::vector<std::string>& getLifetimeParams() const {
    return lifetimeParams_;
  }
  void setLifetimeParams(std::vector<std::string> names) {
    lifetimeParams_ = std::move(names);
  }

  ClassType(std::string className) : mangledName(std::move(className)) {}

  // Constructor for generic class definition
  ClassType(std::string className, std::vector<std::string> typeParams)
      : mangledName(std::move(className)),
        typeParameters(std::move(typeParams)) {}

  // Constructor for specialized generic class
  ClassType(std::string mangledName, std::string baseName,
            std::vector<TypePtr> typeArgs)
      : mangledName(std::move(mangledName)),
        typeArguments(std::move(typeArgs)),
        baseGenericName(std::move(baseName)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Class;
  Kind getKind() const override { return StaticKind; }
  const std::string& getMangledName() const { return mangledName; }

  // Base name accessors (user-written name for error messages)
  const std::string& getBaseName() const {
    return baseName_.empty() ? mangledName : baseName_;
  }
  void setBaseName(std::string bn) { baseName_ = std::move(bn); }
  bool hasBaseName() const { return !baseName_.empty(); }

  // Qualified name accessors
  const sun::QualifiedName& getQualifiedName() const { return qualifiedName_; }
  void setQualifiedName(sun::QualifiedName qn) {
    qualifiedName_ = std::move(qn);
  }
  bool hasQualifiedName() const { return !qualifiedName_.baseName.empty(); }

  // Get user-friendly display name for error messages
  // For specialized classes: "Vec<i32>" or "std.Vec<i32>"
  // For non-specialized: baseName or name with underscores converted to dots
  std::string getDisplayName() const {
    // Prefer the structured name: QualifiedName::display() spells the scope
    // path with dots and drops bundle hash segments. A specialization shows
    // the generic it came from, with its arguments spelled out.
    std::string base;
    if (!baseName_.empty()) {
      base = baseName_;
    } else if (!genericQualifiedName_.empty()) {
      base = genericQualifiedName_.display();
    } else if (hasQualifiedName()) {
      base = qualifiedName_.display();
    } else {
      base = mangledName;
    }
    if (isSpecialized() && !typeArguments.empty()) {
      std::string result = base + "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toDisplayString();
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
  // Qualified name of the generic this specialization was instantiated from
  // (scope path + plain base name), for scope-tree lookups
  const sun::QualifiedName& getGenericQualifiedName() const {
    return genericQualifiedName_;
  }
  void setGenericQualifiedName(sun::QualifiedName qn) {
    genericQualifiedName_ = std::move(qn);
  }
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

  // Returns the new record so callers can set its access info.
  ClassField& addField(const std::string& fieldName, TypePtr fieldType) {
    // Caller should check hasField() first and report error with position
    fields.push_back({fieldName, std::move(fieldType), fields.size()});
    return fields.back();
  }

  ClassMethod& addMethod(const std::string& methodName, TypePtr returnType,
                         std::vector<TypePtr> paramTypes,
                         bool isConstructor = false,
                         std::vector<std::string> typeParams = {},
                         bool canThrow = false) {
    methods.push_back({methodName, std::move(typeParams), std::move(returnType),
                       std::move(paramTypes), isConstructor, canThrow});
    return methods.back();
  }

  void addImplementedInterface(const std::string& interfaceName) {
    if (implementsInterface(interfaceName)) return;
    implementedInterfaces.push_back(interfaceName);
  }

  bool implementsInterface(const std::string& interfaceName) const {
    for (const auto& iface : implementedInterfaces) {
      if (iface == interfaceName) return true;
    }
    return false;
  }

  // An interface implemented with a covariant (class-typed) return where the
  // interface declares an interface type cannot be dispatched through a fat
  // pointer (the ABI differs), so the class is not convertible to it. It is
  // still usable statically (e.g. IIterable for for-in).
  void markStaticOnlyInterface(const std::string& interfaceName) {
    if (!isStaticOnlyInterface(interfaceName)) {
      staticOnlyInterfaces.push_back(interfaceName);
    }
  }
  bool isStaticOnlyInterface(const std::string& interfaceName) const {
    for (const auto& iface : staticOnlyInterfaces) {
      if (iface == interfaceName) return true;
    }
    return false;
  }
  // Class value/ref may be converted to an interface fat pointer
  bool convertibleToInterface(const std::string& interfaceName) const {
    return implementsInterface(interfaceName) &&
           !isStaticOnlyInterface(interfaceName);
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

  // Returns true if an array argument of type `from` is compatible with an
  // array parameter of type `to` (same element type, with an unsized parameter
  // accepting any sized array). Mirrors the array coercion allowed elsewhere so
  // that e.g. array<i32, 3, 2> can be passed where array<i32> is expected.
  static bool isArrayCompatible(const TypePtr& from, const TypePtr& to) {
    if (!from || !to || !from->isArray() || !to->isArray()) {
      return false;
    }
    auto* toArr = static_cast<const ArrayType*>(to.get());
    auto* fromArr = static_cast<const ArrayType*>(from.get());
    return toArr->isCompatibleWith(*fromArr);
  }

  // Returns true if a value of type `from` can be implicitly widened to type
  // `to` (integer-to-wider-integer or float-to-wider-float). This mirrors the
  // numeric widening allowed by free-function overload resolution so that
  // method/constructor overloads accept the same arguments (e.g. an i32 literal
  // passed where an i64 parameter is expected).
  static bool isNumericWidenable(const TypePtr& from, const TypePtr& to) {
    if (!from || !to || !from->isPrimitive() || !to->isPrimitive()) {
      return false;
    }
    auto fromKind = from->getKind();
    auto toKind = to->getKind();

    auto intBitWidth = [](Type::Kind k) -> int {
      switch (k) {
        case Type::Kind::Int8:
        case Type::Kind::UInt8:
          return 8;
        case Type::Kind::Int16:
        case Type::Kind::UInt16:
          return 16;
        case Type::Kind::Int32:
        case Type::Kind::UInt32:
          return 32;
        case Type::Kind::Int64:
        case Type::Kind::UInt64:
          return 64;
        default:
          return 0;
      }
    };

    int fromWidth = intBitWidth(fromKind);
    int toWidth = intBitWidth(toKind);
    if (fromWidth != 0 && toWidth != 0) {
      return fromWidth <= toWidth;
    }

    bool fromFloat =
        fromKind == Type::Kind::Float32 || fromKind == Type::Kind::Float64;
    bool toFloat =
        toKind == Type::Kind::Float32 || toKind == Type::Kind::Float64;
    return fromFloat && toFloat;
  }

  // Get method with overload resolution based on argument types
  // Returns the method whose parameter types best match the provided arg types
  const ClassMethod* getMethodForArgs(
      const std::string& methodName,
      const std::vector<TypePtr>& argTypes) const {
    const ClassMethod* bestMatch = nullptr;
    bool foundExact = false;

    for (const auto& method : methods) {
      if (method.name != methodName) continue;
      if (method.paramTypes.size() != argTypes.size()) continue;

      bool allMatch = true;
      bool allExact = true;
      for (size_t i = 0; i < argTypes.size(); ++i) {
        if (!argTypes[i] || !method.paramTypes[i]) {
          allMatch = false;
          break;
        }

        // Exact type match
        if (method.paramTypes[i]->equals(*argTypes[i])) {
          continue;
        }
        allExact = false;

        // A static_ptr argument narrows to a raw_ptr parameter of the same
        // pointee: the data pointer is passed. Never the other way around.
        if (method.paramTypes[i]->isRawPointer() &&
            argTypes[i]->isStaticPointer()) {
          auto* r =
              static_cast<const RawPointerType*>(method.paramTypes[i].get());
          auto* s = static_cast<const StaticPointerType*>(argTypes[i].get());
          if (s->getPointeeType()->equals(*r->getPointeeType())) {
            continue;
          }
        }

        // Reference parameter accepts the referenced type
        if (method.paramTypes[i]->isReference()) {
          auto* refType =
              static_cast<const ReferenceType*>(method.paramTypes[i].get());
          const TypePtr& referenced = refType->getReferencedType();
          if (referenced->equals(*argTypes[i])) {
            continue;
          }
          // A borrow of the other mutability: only ref -> const ref
          if (argTypes[i]->isReference()) {
            auto* argRef = static_cast<const ReferenceType*>(argTypes[i].get());
            if (refMutabilityConvertible(*argRef, *refType) &&
                referenced->equals(*argRef->getReferencedType())) {
              continue;
            }
          }
          // ref to an (unsized) array accepts a compatible sized array, e.g.
          // passing array<i32, 3, 2> where ref array<i32> is expected.
          if (isArrayCompatible(argTypes[i], referenced)) {
            continue;
          }
        }

        // Array compatibility for by-value array parameters (sized -> unsized).
        if (isArrayCompatible(argTypes[i], method.paramTypes[i])) {
          continue;
        }

        // Numeric widening: e.g. an i32 literal argument for an i64 parameter
        if (isNumericWidenable(argTypes[i], method.paramTypes[i])) {
          continue;
        }

        // Lambda widening: non-throwing where throwing is expected, and
        // environment-free where '<'_>' is expected
        if (method.paramTypes[i]->isLambda() && argTypes[i]->isLambda()) {
          auto* paramL =
              static_cast<const LambdaType*>(method.paramTypes[i].get());
          auto* argL = static_cast<const LambdaType*>(argTypes[i].get());
          if (paramL->acceptsValueOf(*argL)) {
            continue;
          }
        }

        // No match for this parameter
        allMatch = false;
        break;
      }

      if (allMatch) {
        // Prefer exact matches over ref-compatible matches
        if (allExact) {
          return &method;  // Exact match - return immediately
        }
        if (!foundExact) {
          bestMatch = &method;
        }
      }
    }

    return bestMatch;
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
      std::string result = mangledName + "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
      }
      result += ">";
      return result;
    }
    return mangledName;
  }

  std::string toDisplayString() const override { return getDisplayName(); }

  bool equals(const Type& other) const override {
    if (auto* c = dynamic_cast<const ClassType*>(&other)) {
      // For specialized types, compare by mangled name
      return mangledName == c->mangledName;
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

    // Several ClassType objects can describe the same class (e.g. a type
    // resolved inside a .moon and the same type seen by its importer). The
    // mangled name identifies the layout, so share one LLVM struct per name
    // rather than letting LLVM mint "name.1", "name.2" duplicates that then
    // fail to match across call boundaries.
    llvm::StructType* existing =
        llvm::StructType::getTypeByName(ctx, mangledName + "_struct");
    if (existing && !existing->isOpaque()) {
      cachedLLVMType = existing;
      return cachedLLVMType;
    }

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
    if (existing) {
      // Opaque placeholder minted by the module linker: fill it in
      existing->setBody(fieldTypes, isPacked_);
      cachedLLVMType = existing;
      return cachedLLVMType;
    }
    cachedLLVMType = llvm::StructType::create(
        ctx, fieldTypes, mangledName + "_struct", isPacked_);
    return cachedLLVMType;
  }

  // Packed classes have no inter-field padding and struct alignment 1.
  // Must be set before the first getStructType() call, which memoizes.
  bool isPacked() const { return isPacked_; }
  void setPacked(bool v) { isPacked_ = v; }

  // Get mangled method name: ClassName_methodName
  // Class name already includes module path and library hash
  std::string getMangledMethodName(const std::string& methodName) const {
    return mangledName + "_" + methodName;
  }

  // Get mangled method name with parameter types for overload disambiguation
  // Delegates to QualifiedName::buildParamSuffix for consistent mangling
  std::string getMangledMethodName(
      const std::string& methodName,
      const std::vector<TypePtr>& paramTypes) const {
    return mangledName + "_" + methodName +
           QualifiedName::buildParamSuffix(paramTypes);
  }

  // --- ScopeMethodTable accessors ---
  ScopeMethodTable& getMethodTable() { return methodTable_; }
  const ScopeMethodTable& getMethodTable() const { return methodTable_; }

  // Register a method in the indexed method table (by signature)
  void registerMethod(const std::string& sig, ScopeMethodInfo info) {
    methodTable_[sig] = std::move(info);
  }

  // Look up a method by exact signature
  const ScopeMethodInfo* lookupMethod(const std::string& sig) const {
    auto it = methodTable_.find(sig);
    if (it != methodTable_.end()) return &it->second;
    return nullptr;
  }

  // Get all overloads for a method name
  const std::vector<ScopeMethodInfo*>* getMethodOverloads(
      const std::string& methodName) const {
    return methodTable_.getOverloads(methodName);
  }
};

// Interface field information
struct InterfaceField {
  std::string name;
  TypePtr type;
  sun::Visibility visibility = sun::Visibility::Private;
};

// Interface method information
struct InterfaceMethod {
  std::string name;
  std::vector<std::string> typeParameters;  // Generic type params: <T, U>
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;  // Excludes implicit 'this' parameter
  bool hasDefaultImpl;   // true if this method has a default implementation
  bool isConst = false;  // `const function`: does not change `this`
  sun::Visibility visibility = sun::Visibility::Private;

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
  ScopeMethodTable
      methodTable_;  // Indexed method table for default implementations
  sun::QualifiedName qualifiedName_;
  // Lifetime names the interface DECLARES ('interface ISink<'a>').
  // Declarations only, never bindings - see ClassType::lifetimeParams_.
  std::vector<std::string> lifetimeParams_;

  sun::QualifiedName genericQualifiedName_;

 public:
  /** The original template name, independent of specialization and source
   * aliases. */
  const sun::QualifiedName& getGenericQualifiedName() const {
    return genericQualifiedName_;
  }
  /** Record the template that produced this type. */
  void setGenericQualifiedName(sun::QualifiedName name) {
    genericQualifiedName_ = std::move(name);
  }

  sun::Visibility visibility = sun::Visibility::Private;

  const std::vector<std::string>& getLifetimeParams() const {
    return lifetimeParams_;
  }
  void setLifetimeParams(std::vector<std::string> names) {
    lifetimeParams_ = std::move(names);
  }

  InterfaceType(std::string interfaceName) : name(std::move(interfaceName)) {}

  // Structured name: owner() is the declaring module (unit of visibility)
  const sun::QualifiedName& getQualifiedName() const { return qualifiedName_; }
  void setQualifiedName(sun::QualifiedName qn) {
    qualifiedName_ = std::move(qn);
  }

  // Constructor for generic interface definition
  InterfaceType(std::string interfaceName, std::vector<std::string> typeParams)
      : name(std::move(interfaceName)), typeParameters(std::move(typeParams)) {}

  // Constructor for specialized generic interface
  InterfaceType(std::string mangledName, std::string baseName,
                std::vector<TypePtr> typeArgs)
      : name(std::move(mangledName)),
        typeArguments(std::move(typeArgs)),
        baseGenericName(std::move(baseName)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Interface;
  Kind getKind() const override { return StaticKind; }
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

  // Returns the (possibly pre-existing) record so callers can set access.
  InterfaceField& addField(const std::string& fieldName, TypePtr fieldType) {
    for (auto& existingField : fields) {
      if (existingField.name == fieldName) return existingField;
    }
    fields.push_back({fieldName, std::move(fieldType)});
    return fields.back();
  }

  InterfaceMethod& addMethod(const std::string& methodName, TypePtr returnType,
                             std::vector<TypePtr> paramTypes,
                             bool hasDefaultImpl = false,
                             std::vector<std::string> typeParams = {}) {
    methods.push_back({methodName, std::move(typeParams), std::move(returnType),
                       std::move(paramTypes), hasDefaultImpl});
    return methods.back();
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

  // Rebind one method's return type. Exists for the builtin IError: it is
  // registered before any source is read, so message() starts as
  // static_ptr<u8> and is retargeted to the String class when the stdlib
  // registers one (see SemanticAnalyzer::registerClassShape).
  void setMethodReturnType(const std::string& methodName, TypePtr returnType) {
    for (auto& method : methods) {
      if (method.name == methodName) {
        method.returnType = std::move(returnType);
        return;
      }
    }
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

  std::string toDisplayString() const override {
    std::string base;
    if (!baseName_.empty()) {
      base = baseName_;
    } else if (!baseGenericName.empty()) {
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
        result += typeArguments[i]->toDisplayString();
      }
      result += ">";
      return result;
    }
    return base;
  }

  bool equals(const Type& other) const override {
    if (auto* i = dynamic_cast<const InterfaceType*>(&other)) {
      return name == i->name;
    }
    return false;
  }

  // Interfaces are represented as fat pointers: { ptr data, ptr vtable }.
  // The vtable contains method pointers followed by concrete drop glue (or a
  // no-op for a borrow), so an owning interface remains two pointers.
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

  // --- ScopeMethodTable accessors ---
  ScopeMethodTable& getMethodTable() { return methodTable_; }
  const ScopeMethodTable& getMethodTable() const { return methodTable_; }

  // Register a method in the indexed method table (by signature)
  void registerMethod(const std::string& sig, ScopeMethodInfo info) {
    methodTable_[sig] = std::move(info);
  }

  // Look up a method by exact signature
  const ScopeMethodInfo* lookupMethod(const std::string& sig) const {
    auto it = methodTable_.find(sig);
    if (it != methodTable_.end()) return &it->second;
    return nullptr;
  }

  // Get all overloads for a method name
  const std::vector<ScopeMethodInfo*>* getMethodOverloads(
      const std::string& methodName) const {
    return methodTable_.getOverloads(methodName);
  }
};

// Enum variant information
struct EnumVariant {
  std::string name;
  int64_t value;  // Numeric value of the variant (the runtime tag)
  std::vector<TypePtr> payloadTypes;  // empty = unit variant

  bool hasPayload() const { return !payloadTypes.empty(); }
};

// Forward declaration for EnumType
class EnumType;
using EnumTypePtr = std::shared_ptr<EnumType>;

// Enum type for user-defined enums
// Enums are represented as i32 values, with variants as named constants
// Example: enum Color { Red, Green, Blue }
class EnumType : public Type {
  std::string mangledName_;  // Mangled name (e.g., "$hash$_std_Color")
  std::string baseName_;     // User-written base name (e.g., "Color")
  std::vector<EnumVariant> variants;
  std::string genericBase_;           // e.g. "Option" for Option_i32
  std::vector<TypePtr> genericArgs_;  // e.g. [i32] for Option_i32
  sun::QualifiedName qualifiedName_;

  sun::QualifiedName genericQualifiedName_;

 public:
  /** The original template name, independent of specialization and source
   * aliases. */
  const sun::QualifiedName& getGenericQualifiedName() const {
    return genericQualifiedName_;
  }
  /** Record the template that produced this type. */
  void setGenericQualifiedName(sun::QualifiedName name) {
    genericQualifiedName_ = std::move(name);
  }

  sun::Visibility visibility = sun::Visibility::Private;

  // Structured name: owner() is the declaring module (unit of visibility)
  const sun::QualifiedName& getQualifiedName() const { return qualifiedName_; }
  void setQualifiedName(sun::QualifiedName qn) {
    qualifiedName_ = std::move(qn);
  }

  EnumType(std::string mangledName, std::string baseName = "")
      : mangledName_(std::move(mangledName)), baseName_(std::move(baseName)) {}

  EnumType(std::string mangledName, std::vector<EnumVariant> vars,
           std::string baseName = "")
      : mangledName_(std::move(mangledName)),
        variants(std::move(vars)),
        baseName_(std::move(baseName)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Enum;
  Kind getKind() const override { return StaticKind; }
  const std::string& getName() const { return mangledName_; }
  const std::vector<EnumVariant>& getVariants() const { return variants; }

  // Base name accessor
  const std::string& getBaseName() const {
    return baseName_.empty() ? mangledName_ : baseName_;
  }
  bool hasBaseName() const { return !baseName_.empty(); }
  void setBaseName(std::string baseName) { baseName_ = std::move(baseName); }

  // Generic specialization origin (e.g. Option_i32 records base "Option" and
  // args [i32]); empty for non-generic enums.
  void setGenericOrigin(std::string base, std::vector<TypePtr> args) {
    genericBase_ = std::move(base);
    genericArgs_ = std::move(args);
  }
  const std::string& getGenericBase() const { return genericBase_; }
  const std::vector<TypePtr>& getGenericArgs() const { return genericArgs_; }
  bool isGenericSpecialization() const { return !genericBase_.empty(); }

  // Get user-friendly display name for error messages
  std::string getDisplayName() const {
    if (isGenericSpecialization()) {
      std::string result = genericBase_ + "<";
      for (size_t i = 0; i < genericArgs_.size(); ++i) {
        if (i > 0) result += ", ";
        result += genericArgs_[i]->toDisplayString();
      }
      return result + ">";
    }
    if (!baseName_.empty()) return baseName_;
    if (!qualifiedName_.empty()) return qualifiedName_.display();
    return mangledName_;
  }

  std::string toDisplayString() const override { return getDisplayName(); }

  // Idempotent: declaration collection and full analysis both register
  // variants; the second registration must not duplicate them.
  void addVariant(const std::string& variantName, int64_t value) {
    for (const auto& v : variants) {
      if (v.name == variantName) return;
    }
    variants.push_back({variantName, value, {}});
  }

  // Attach resolved payload types to a variant (full-analysis phase; payload
  // annotations may reference classes not yet registered during declaration
  // collection).
  void setVariantPayloadTypes(const std::string& variantName,
                              std::vector<TypePtr> payloadTypes) {
    for (auto& v : variants) {
      if (v.name == variantName) {
        v.payloadTypes = std::move(payloadTypes);
        return;
      }
    }
    assert(false && "setVariantPayloadTypes: unknown variant");
  }

  // True if any variant carries a payload (tagged-union representation)
  bool hasPayload() const {
    for (const auto& v : variants) {
      if (v.hasPayload()) return true;
    }
    return false;
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

  std::string toString() const override { return mangledName_; }

  bool equals(const Type& other) const override {
    if (auto* e = dynamic_cast<const EnumType*>(&other)) {
      return mangledName_ == e->mangledName_;
    }
    return false;
  }

  // Payload-free enums are represented as i32 values. Payload enums need the
  // module DataLayout for storage sizing: LLVMTypeResolver computes the
  // storage struct and caches it here; afterwards toLLVMType serves the
  // cache (e.g. for class field embedding via ClassType::getStructType).
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    if (hasPayload()) {
      if (cachedStorageType) return cachedStorageType;
      logAndThrowError("payload enum '" + getDisplayName() +
                       "' LLVM type requires DataLayout - resolve it via "
                       "LLVMTypeResolver first");
    }
    return llvm::Type::getInt32Ty(ctx);
  }

  // Get the mangled variant name: EnumName_VariantName
  std::string getMangledVariantName(const std::string& variantName) const {
    return mangledName_ + "_" + variantName;
  }

  // LLVM struct caches, populated by LLVMTypeResolver (mirrors
  // ClassType::cachedLLVMType). storage = { i32 tag, [M x unitTy] }; per
  // variant = { i32 tag, T1, T2, ... } GEP'd on the same base pointer.
  mutable llvm::StructType* cachedStorageType = nullptr;
  mutable std::unordered_map<std::string, llvm::StructType*>
      cachedVariantStructs;
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
  static TypePtr Char() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Char);
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

  // Create a function-pointer type: function () T
  static TypePtr Function(TypePtr returnType, std::vector<TypePtr> paramTypes,
                          bool canThrow = false) {
    return std::make_shared<FunctionType>(std::move(returnType),
                                          std::move(paramTypes), canThrow);
  }

  // Create a lambda type: () => {} (anonymous function, fat pointer call)
  static TypePtr Lambda(TypePtr returnType, std::vector<TypePtr> paramTypes,
                        bool canThrow = false) {
    return std::make_shared<LambdaType>(std::move(returnType),
                                        std::move(paramTypes), canThrow);
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

  // Generate mangled name for a specialized generic class: the generic's
  // mangled name with each type argument spelled the way every symbol
  // spells types (QualifiedName::canonicalTypeString)
  static std::string mangleGenericClassName(
      const std::string& baseName, const std::vector<TypePtr>& typeArgs) {
    std::string result = baseName;
    for (const auto& arg : typeArgs) {
      result += "_" + QualifiedName::canonicalTypeString(arg);
    }
    return result;
  }

  // Create a type parameter type. `constraint` is the name written after the
  // colon in `<T: Trait>`, empty when the parameter is unconstrained.
  static TypePtr TypeParameter(const std::string& name,
                               const std::string& constraint = "") {
    return std::make_shared<TypeParameterType>(name, constraint);
  }

  // `_return_type_of<F>` where F is not bound yet: what F returns, carried as
  // F plus the projection until a specialization says what F is.
  static TypePtr TypeParameterProjection(const std::string& base,
                                         const std::string& constraint,
                                         TypeProjection projection) {
    return std::make_shared<TypeParameterType>(base, constraint, projection);
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
    if (name == "char") return Char();
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

  // Register built-in types (IError). The iteration protocol
  // (IIterator/IIterable) lives in stdlib/iterator.sun since it names Option.
  void registerBuiltins() {
    // Create IError interface with code() and message() methods.
    // message() starts as static_ptr<u8> — the only string type that exists
    // before any source is read. When the stdlib's String class is registered,
    // the return type is retargeted to it (an owned clone of the message), so
    // errors can carry text composed at runtime. Without the stdlib, message()
    // stays literal-only.
    auto ierror = std::make_shared<InterfaceType>("IError");
    // Not const: every user error class would then have to spell
    // `const function code()`, and errors are caught into plain variables.
    ierror->addMethod("code", Types::Int32(), {}, true);
    ierror->addMethod("message", Types::String(), {}, true);
    interfaceCache["IError"] = ierror;
  }

  // Check if a type name is a builtin type that cannot be redefined
  // Includes builtin interfaces and type traits used by _is<T>
  bool isBuiltinTypeName(const std::string& name) const {
    static const std::unordered_set<std::string> builtinNames = {
        // Builtin interfaces
        "IError",
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

  // Get or create a class type by qualified name
  // Sets the qualified name and base name on the class type automatically
  std::shared_ptr<ClassType> getClass(const sun::QualifiedName& qualifiedName) {
    std::string name = qualifiedName.mangled();
    auto classType = getClass(name);
    // Set qualified name if not already set
    if (!classType->hasQualifiedName()) {
      classType->setQualifiedName(qualifiedName);
    }
    // Set base name for error messages if there's a scope path. A
    // specialization derives its display name from the generic's, so leave
    // it alone (its mangled base name would read "Vec_i32<i32>").
    if (!classType->hasBaseName() && !classType->isSpecialized() &&
        !qualifiedName.scopePath.empty()) {
      classType->setBaseName(qualifiedName.baseName);
    }
    return classType;
  }

  // Get or create a class type by mangled name
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
inline bool Type::isCompound() const {
  if (isEnum()) {
    return static_cast<const EnumType*>(this)->hasPayload();
  }
  return !isPrimitive() && !isReference() && !isRawPointer() &&
         !isStaticPointer() && !isFunction() && !isLambda() &&
         !isTypeParameter();
}

// True if dropping a value of this type must run cleanup code: classes with a
// deinit method (directly, or transitively through class/enum-typed fields)
// and payload enums with at least one payload that needs drop.
inline bool typeNeedsDropImpl(const Type* type,
                              std::unordered_set<const Type*>& visited) {
  if (!type || !visited.insert(type).second) return false;
  if (type->isClass()) {
    auto* c = static_cast<const ClassType*>(type);
    if (c->getMethod("deinit")) return true;
    for (const auto& field : c->getFields()) {
      if (field.type && typeNeedsDropImpl(field.type.get(), visited)) {
        return true;
      }
    }
    return false;
  }
  // A by-value interface owns its erased concrete implementation. Its vtable
  // supplies the concrete drop routine; ref Interface values remain borrows.
  if (type->isInterface()) return true;
  if (type->isEnum()) {
    auto* e = static_cast<const EnumType*>(type);
    for (const auto& v : e->getVariants()) {
      for (const auto& pt : v.payloadTypes) {
        if (pt && typeNeedsDropImpl(pt.get(), visited)) return true;
      }
    }
    return false;
  }
  // A sized array owns its elements and drops each of them; an unsized array
  // is a view and owns nothing.
  if (type->isArray()) {
    auto* a = static_cast<const ArrayType*>(type);
    return !a->isUnsized() && a->getElementType() &&
           typeNeedsDropImpl(a->getElementType().get(), visited);
  }
  return false;
}

inline bool typeNeedsDrop(const Type* type) {
  std::unordered_set<const Type*> visited;
  return typeNeedsDropImpl(type, visited);
}

inline bool typeNeedsDrop(const TypePtr& type) {
  return typeNeedsDrop(type.get());
}

// True if a value of this type may carry a lambda environment that lives in
// a stack frame: a '<'_>' lambda, or anything that can transitively hold
// one — a class through its fields or generic type arguments (containers
// hide elements behind raw storage, so the arguments must count), a payload
// enum, an array. Such a value must not outlive the frame it was built in.
// References are the sibling case, tracked by the borrow checker's
// class-stores-refs walk.
inline bool typeIsFrameCarryingImpl(const Type* type,
                                    std::unordered_set<const Type*>& visited) {
  if (!type || !visited.insert(type).second) return false;
  if (type->isLambda()) {
    return static_cast<const LambdaType*>(type)->hasRefCaptures();
  }
  if (type->isClass()) {
    auto* c = static_cast<const ClassType*>(type);
    for (const auto& field : c->getFields()) {
      if (field.type && typeIsFrameCarryingImpl(field.type.get(), visited)) {
        return true;
      }
    }
    for (const auto& arg : c->getTypeArguments()) {
      if (arg && typeIsFrameCarryingImpl(arg.get(), visited)) return true;
    }
    return false;
  }
  if (type->isEnum()) {
    auto* e = static_cast<const EnumType*>(type);
    for (const auto& v : e->getVariants()) {
      for (const auto& pt : v.payloadTypes) {
        if (pt && typeIsFrameCarryingImpl(pt.get(), visited)) return true;
      }
    }
    for (const auto& arg : e->getGenericArgs()) {
      if (arg && typeIsFrameCarryingImpl(arg.get(), visited)) return true;
    }
    return false;
  }
  if (type->isArray()) {
    auto* a = static_cast<const ArrayType*>(type);
    return a->getElementType() &&
           typeIsFrameCarryingImpl(a->getElementType().get(), visited);
  }
  return false;
}

inline bool typeIsFrameCarrying(const Type* type) {
  std::unordered_set<const Type*> visited;
  return typeIsFrameCarryingImpl(type, visited);
}

inline bool typeIsFrameCarrying(const TypePtr& type) {
  return typeIsFrameCarrying(type.get());
}

// A copy of the type with every lifetime NAME stripped, recursively.
// Lifetime names are relative to one signature's lifetime list; a type that
// crosses into another namespace - a generic type-parameter binding, whose
// specialization is shared by every caller - must not carry them along.
// The <'_> marker itself is identity and stays.
inline TypePtr eraseLifetimeNames(const TypePtr& type) {
  if (!type) return type;
  if (auto* lt = dynamic_cast<const LambdaType*>(type.get())) {
    bool named = !lt->getLifetimeName().empty();
    std::vector<TypePtr> params;
    bool changed = named;
    for (const auto& param : lt->getParamTypes()) {
      auto stripped = eraseLifetimeNames(param);
      changed = changed || stripped != param;
      params.push_back(std::move(stripped));
    }
    auto ret = eraseLifetimeNames(lt->getReturnType());
    changed = changed || ret != lt->getReturnType();
    if (!changed) return type;
    auto result = Types::Lambda(ret, std::move(params), lt->canThrow());
    static_cast<LambdaType*>(result.get())
        ->setHasRefCaptures(lt->hasRefCaptures());
    return result;
  }
  if (auto* rt = dynamic_cast<const ReferenceType*>(type.get())) {
    if (rt->getLifetimeName().empty() && rt->getClassLifetimeArgs().empty()) {
      auto referent = eraseLifetimeNames(rt->getReferencedType());
      if (referent == rt->getReferencedType()) return type;
      return Types::Reference(referent, rt->isMutable());
    }
    return Types::Reference(eraseLifetimeNames(rt->getReferencedType()),
                            rt->isMutable());
  }
  return type;
}

// True if a read can honestly duplicate a value of this type. Scalars can:
// primitives, pointers, functions. A class, payload enum, interface or array
// value cannot: it has one owner, so reading one out of a borrow would hand
// back a second value backed by the borrowed storage. Borrow it with `ref`
// instead, or copy it explicitly with a clone method. Unbound type parameters
// answer true; the specialization is checked with the concrete type in hand.
inline bool typeCopiesByRead(const Type* type) {
  return type && !type->isCompound();
}

inline bool typeCopiesByRead(const TypePtr& type) {
  return typeCopiesByRead(type.get());
}

// True if reading a value of this type out of a place MOVES it: an owned
// compound value. A borrow stays put and a scalar copies.
inline bool typeMovesOnRead(const Type* type) {
  return type && !type->isReference() && !typeCopiesByRead(type);
}

inline bool typeMovesOnRead(const TypePtr& type) {
  return typeMovesOnRead(type.get());
}

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
  // NOT compatible with static_ptr in either direction here: equals is used
  // symmetrically, and only one direction is sound. The static_ptr → raw_ptr
  // narrowing lives in the explicit conversion rules (isAssignableTo, the
  // overload matchers, and analyzeCall).
  if (auto* p = dynamic_cast<const RawPointerType*>(&other)) {
    return pointeeType->equals(*p->pointeeType);
  }
  return false;
}

}  // namespace sun
