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

#include "ast/type_constraint.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "semantic_analysis/declaration_table.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/struct_names.h"
#include "semantic_analysis/visibility.h"
#include "support/error.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::ast::TypeConstraint;
using sun::support::logAndThrowError;

// Forward declarations
class Type;
class ArrayType;  // Forward declared for ReferenceType
/** Shared ownership of a semantic type description. */
using TypePtr = std::shared_ptr<Type>;

/** Compare semantic types exactly, without accepting implicit conversions. */
bool sameTypeIdentity(const TypePtr& left, const TypePtr& right);
/** Compare ordered parameter or specialization argument types exactly. */
bool sameTypeArguments(const std::vector<TypePtr>& left,
                       const std::vector<TypePtr>& right);

/**
 * Base Type class
 */
class Type {
 public:
  /** Identifies the primitive and composite type categories understood by the compiler. */
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

  /** Destroys this object and releases its owned members. */
  virtual ~Type() = default;
  /** Returns the type category used for semantic checks and dispatch. */
  virtual Kind getKind() const = 0;
  /** Returns a readable representation for diagnostics and debugging. */
  virtual std::string toString() const = 0;
  /**
   * User-friendly name for error messages (strips internal prefixes)
   */
  virtual std::string toDisplayString() const { return toString(); }
  /** Reports whether the other type has the same semantic identity. */
  virtual bool equals(const Type& other) const = 0;
  /** Returns the LLVM type used to represent values of this semantic type. */
  virtual llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const = 0;

  /**
   * Convenience checks for primitive types
   */
  bool isVoid() const { return getKind() == Kind::Void; }
  /** Reports whether this type represents bool values. */
  bool isBool() const { return getKind() == Kind::Bool; }
  /** Reports whether this type represents int8 values. */
  bool isInt8() const { return getKind() == Kind::Int8; }
  /** Reports whether this type represents int16 values. */
  bool isInt16() const { return getKind() == Kind::Int16; }
  /** Reports whether this type represents int32 values. */
  bool isInt32() const { return getKind() == Kind::Int32; }
  /** Reports whether this type represents int64 values. */
  bool isInt64() const { return getKind() == Kind::Int64; }
  /** Reports whether this type represents u int8 values. */
  bool isUInt8() const { return getKind() == Kind::UInt8; }
  /** Reports whether this type represents u int16 values. */
  bool isUInt16() const { return getKind() == Kind::UInt16; }
  /** Reports whether this type represents u int32 values. */
  bool isUInt32() const { return getKind() == Kind::UInt32; }
  /** Reports whether this type represents u int64 values. */
  bool isUInt64() const { return getKind() == Kind::UInt64; }
  /** Reports whether this type represents float32 values. */
  bool isFloat32() const { return getKind() == Kind::Float32; }
  /** Reports whether this type represents float64 values. */
  bool isFloat64() const { return getKind() == Kind::Float64; }
  /** Reports whether this type represents char values. */
  bool isChar() const { return getKind() == Kind::Char; }
  /** Reports whether this type represents signed values. */
  bool isSigned() const {
    Kind k = getKind();
    return k == Kind::Int8 || k == Kind::Int16 || k == Kind::Int32 ||
           k == Kind::Int64;
  }
  /** Reports whether this type represents unsigned values. */
  bool isUnsigned() const {
    Kind k = getKind();
    return k == Kind::UInt8 || k == Kind::UInt16 || k == Kind::UInt32 ||
           k == Kind::UInt64;
  }
  /**
   * `char` is primitive (passed by value, no ownership) but deliberately not
   * numeric or integral: arithmetic and implicit widening are keyed off
   * isNumeric()/isIntegral(), so leaving it out of those rejects `'a' + 1`
   * and any silent char/integer mixing.
   */
  bool isPrimitive() const {
    Kind k = getKind();
    return k == Kind::Void || k == Kind::Bool || k == Kind::Int8 ||
           k == Kind::Int16 || k == Kind::Int32 || k == Kind::Int64 ||
           k == Kind::UInt8 || k == Kind::UInt16 || k == Kind::UInt32 ||
           k == Kind::UInt64 || k == Kind::Float32 || k == Kind::Float64 ||
           k == Kind::Char;
  }
  /**
   * Convenience checks for composite types
   */
  bool isFunction() const { return getKind() == Kind::Function; }
  /** Reports whether this type represents lambda values. */
  bool isLambda() const { return getKind() == Kind::Lambda; }
  /** Reports whether this type represents raw pointer values. */
  bool isRawPointer() const { return getKind() == Kind::RawPointer; }
  /** Reports whether this type represents static pointer values. */
  bool isStaticPointer() const { return getKind() == Kind::StaticPointer; }
  /** Reports whether this type represents null pointer values. */
  bool isNullPointer() const { return getKind() == Kind::NullPointer; }
  /** Reports whether this type represents reference values. */
  bool isReference() const { return getKind() == Kind::Reference; }

  /**
   * Returns true for any pointer-like type (raw or static)
   */
  bool isAnyPointer() const { return isRawPointer() || isStaticPointer(); }
  /** Reports whether this type represents class values. */
  bool isClass() const { return getKind() == Kind::Class; }
  /** Reports whether this type represents interface values. */
  bool isInterface() const { return getKind() == Kind::Interface; }
  /** Reports whether this type represents enum values. */
  bool isEnum() const { return getKind() == Kind::Enum; }
  /** Reports whether this type represents module values. */
  bool isModule() const { return getKind() == Kind::Module; }
  /** Reports whether this type represents type parameter values. */
  bool isTypeParameter() const { return getKind() == Kind::TypeParameter; }
  /** Reports whether this type represents error union values. */
  bool isErrorUnion() const { return getKind() == Kind::ErrorUnion; }
  /** Reports whether this type represents array values. */
  bool isArray() const { return getKind() == Kind::Array; }
  /** Reports whether this type represents slice values. */
  bool isSlice() const { return getKind() == Kind::Slice; }
  /** Reports whether this value can be invoked as a function. */
  bool isCallable() const { return isFunction() || isLambda(); }
  /**
   * Compound types must be passed by reference (classes, interfaces, arrays,
   * payload-carrying enums). Payload-free enums are NOT compound - they are
   * i32 values and passed by value. Defined out-of-line (needs EnumType).
   */
  bool isCompound() const;
  /** Reports whether this type represents numeric values. */
  bool isNumeric() const;
  /** Reports whether this type represents integral values. */
  bool isIntegral() const;
  /** Reports whether this type represents floating point values. */
  bool isFloatingPoint() const;
  /** Reports whether this type represents string values. */
  bool isString() const;
};

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
 * Reports whether reading this type can duplicate its value without transferring
 * ownership.
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
 * Something computed from a type parameter rather than the parameter itself.
 * `_return_type_of<F>` names a type that is only known once F is, so until
 * then it travels as the parameter F plus the projection to apply to it.
 */
enum class TypeProjection : uint8_t {
  None,        // the parameter itself
  ReturnType,  // _return_type_of<F>: what F returns
};

/**
 * Type parameter (used in generic class/function definitions)
 * Represents a type variable like T, U, V in class List<T>
 */
class TypeParameterType : public Type {
  std::string name;   // Parameter name: T, U, etc. (F$ret when projected)
  std::string base_;  // The parameter the projection applies to
  TypeProjection projection_ = TypeProjection::None;
  DeclarationId declaration_;
  std::shared_ptr<const int> session_;
  // What `<T: Trait>` promised about whatever T stands for. Metadata only —
  // intentionally excluded from equals()/toString() so it never disturbs
  // substitution or identity. It travels with the parameter so a body being
  // analyzed with T still standing for itself can see what T is known to be.
  TypeConstraint constraint_;

 public:
  /** Creates a generic type parameter retaining its constraint and declaration identity. */
  explicit TypeParameterType(std::string paramName,
                             TypeConstraint constraint = {},
                             TypeProjection projection = TypeProjection::None,
                             DeclarationId declaration = {},
                             std::shared_ptr<const int> session = {})
      : name(paramName),
        base_(std::move(paramName)),
        projection_(projection),
        declaration_(declaration),
        session_(std::move(session)),
        constraint_(std::move(constraint)) {
    // Give projected parameters a distinct diagnostic spelling.
    if (projection_ == TypeProjection::ReturnType) name = base_ + "$ret";
  }

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::TypeParameter;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }
  /** Returns the projection base stored by this object. */
  const std::string& getProjectionBase() const { return base_; }
  /** Returns the projection stored by this object. */
  TypeProjection getProjection() const { return projection_; }
  /** The complete requirement carried by this parameter. */
  const TypeConstraint& getConstraint() const { return constraint_; }
  /** Reports whether this object has constraint. */
  bool hasConstraint() const { return !constraint_.name.empty(); }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return name; }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    return projection_ == TypeProjection::ReturnType
               ? "_return_type_of<" + base_ + ">"
               : name;
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* p = dynamic_cast<const TypeParameterType*>(&other)) {
      if (!declaration_ || !p->declaration_) return this == p;
      return declaration_ == p->declaration_ && session_ == p->session_ &&
             projection_ == p->projection_;
    }
    return false;
  }

  /** Retain the binder identity when projecting its eventual concrete type. */
  TypePtr project(TypeProjection projection) const {
    return std::make_shared<TypeParameterType>(base_, constraint_, projection,
                                               declaration_, session_);
  }

  /**
   * Type parameters can't be directly converted to LLVM types
   * They must be substituted first
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    // This should never be called for unsubstituted type parameters
    assert(false && "Cannot convert type parameter to LLVM type");
    return nullptr;
  }
};

/**
 * Primitive types (int, float, bool, etc.)
 */
class PrimitiveType : public Type {
  Kind kind;

 public:
  /** Creates the semantic descriptor for a primitive type category. */
  explicit PrimitiveType(Kind k) : kind(k) {}

  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return kind; }

  /** Returns a readable representation for diagnostics and debugging. */
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

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    return kind == other.getKind();
  }

  /** Returns the LLVM type used to represent values of this semantic type. */
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

/**
 * A non-null, one-word pointer to a module-scope function.
 * Type annotation: function (Args) Result
 */
class FunctionType : public Type {
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;
  bool requiresUnsafe_ = false;
  bool canThrow_ = false;  // declared with 'throws IError' — may unwind

 public:
  /** Creates a function type from its result, parameters, and calling restrictions. */
  FunctionType(TypePtr ret, std::vector<TypePtr> params, bool canThrow = false,
               bool requiresUnsafe = false)
      : returnType(std::move(ret)),
        paramTypes(std::move(params)),
        requiresUnsafe_(requiresUnsafe),
        canThrow_(canThrow) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Function;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the semantic type of the function result. */
  const TypePtr& getReturnType() const { return returnType; }
  /** Provides the ordered semantic types of the function parameters. */
  const std::vector<TypePtr>& getParamTypes() const { return paramTypes; }

  /** Whether calling this value requires an unsafe block. */
  bool requiresUnsafe() const { return requiresUnsafe_; }
  /**
   * Whether calls through this pointer may throw.
   */
  bool canThrow() const { return canThrow_; }
  /** Updates the can throw stored by this object. */
  void setCanThrow(bool v) { canThrow_ = v; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = "function (";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toString();
    }
    result += ") " + returnType->toString();
    if (requiresUnsafe_) result = "unsafe " + result;
    if (canThrow_) result += " throws IError";
    return result;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    std::string result = "function (";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toDisplayString();
    }
    result += ") " + returnType->toDisplayString();
    if (requiresUnsafe_) result = "unsafe " + result;
    if (canThrow_) result += " throws IError";
    return result;
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* f = dynamic_cast<const FunctionType*>(&other)) {
      if (requiresUnsafe_ != f->requiresUnsafe_) return false;
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

  /**
   * Function values use LLVM's opaque pointer representation.
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::PointerType::getUnqual(ctx);
  }

  /**
   * Returns a pointer to the function type (for function pointer variables)
   */
  llvm::Type* toPointerType(llvm::LLVMContext& ctx) const {
    return toLLVMType(ctx);
  }

  /**
   * Get the raw LLVM FunctionType (for indirect calls)
   */
  llvm::FunctionType* toLLVMFunctionType(llvm::LLVMContext& ctx) const {
    std::vector<llvm::Type*> llvmParams;
    for (const auto& p : paramTypes) {
      llvmParams.push_back(p->toLLVMType(ctx));
    }
    return llvm::FunctionType::get(returnType->toLLVMType(ctx), llvmParams,
                                   false);
  }

  /**
   * Get the closure struct type { ptr, ptr } (func*, env*)
   */
  llvm::StructType* toLLVMClosureType(llvm::LLVMContext& ctx) const {
    return llvm::StructType::get(
        ctx, {
                 llvm::PointerType::getUnqual(ctx),  // func*
                 llvm::PointerType::getUnqual(ctx)   // env*
             });
  }
};

/**
 * Lambda type for anonymous functions (fat pointer call, returnable)
 * Type annotation: () => {}
 */
class LambdaType : public Type {
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;
  bool requiresUnsafe_ = false;
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
  // specialization identity ignore them - '<'a>' and '<'b>' are one type.
  std::string lifetimeName_;

 public:
  /** Creates a captured-callable type from its result, parameters, and restrictions. */
  LambdaType(TypePtr ret, std::vector<TypePtr> params, bool canThrow = false,
             bool requiresUnsafe = false)
      : returnType(std::move(ret)),
        paramTypes(std::move(params)),
        requiresUnsafe_(requiresUnsafe),
        canThrow_(canThrow) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Lambda;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the semantic type of the function result. */
  const TypePtr& getReturnType() const { return returnType; }
  /** Provides the ordered semantic types of the function parameters. */
  const std::vector<TypePtr>& getParamTypes() const { return paramTypes; }
  /** Whether calling this value requires an unsafe block. */
  bool requiresUnsafe() const { return requiresUnsafe_; }
  /** Reports whether calls through this type may produce an error. */
  bool canThrow() const { return canThrow_; }
  /** Reports whether this object has ref captures. */
  bool hasRefCaptures() const { return hasRefCaptures_; }
  /** Updates the has ref captures stored by this object. */
  void setHasRefCaptures(bool v) { hasRefCaptures_ = v; }
  /** Returns the named lifetime associated with the borrowed value. */
  const std::string& getLifetimeName() const { return lifetimeName_; }
  /** Assigns the named lifetime associated with the borrowed value. */
  void setLifetimeName(std::string name) { lifetimeName_ = std::move(name); }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = hasRefCaptures_ ? "<'_>(" : "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toString();
    }
    result += ") => " + returnType->toString();
    if (requiresUnsafe_) result = "unsafe " + result;
    if (canThrow_) result += " throws IError";
    return result;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    std::string result = hasRefCaptures_ ? "<'_>(" : "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toDisplayString();
    }
    result += ") => " + returnType->toDisplayString();
    if (requiresUnsafe_) result = "unsafe " + result;
    if (canThrow_) result += " throws IError";
    return result;
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* l = dynamic_cast<const LambdaType*>(&other)) {
      if (requiresUnsafe_ != l->requiresUnsafe_) return false;
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

  /**
   * Same signature ignoring throwing-ness and the lifetime marker. Used by the
   * bound-method overload chooser, which picks a method by shape before the
   * chosen value is flagged as frame-bound; assignability rejects it later.
   */
  bool equalsIgnoringThrow(const LambdaType& other) const {
    if (!returnType->equals(*other.returnType)) return false;
    if (paramTypes.size() != other.paramTypes.size()) return false;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (!paramTypes[i]->equals(*other.paramTypes[i])) return false;
    }
    return true;
  }

  /**
   * Can a by-value 'from' argument bind to a parameter of this type?
   * Same signature, and each marker only widens: a non-throwing lambda may
   * go where a throwing one is expected, and an environment-free lambda may
   * go where a '<'_>' one is expected — never the other way around.
   */
  bool acceptsValueOf(const LambdaType& from) const {
    if (!equalsIgnoringThrow(from)) return false;
    if (!requiresUnsafe_ && from.requiresUnsafe_) return false;
    if (!canThrow_ && from.canThrow_) return false;
    if (!hasRefCaptures_ && from.hasRefCaptures_) return false;
    return true;
  }

  /**
   * Returns the closure struct type { ptr, ptr } (func*, env*)
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::StructType::get(
        ctx, {
                 llvm::PointerType::getUnqual(ctx),  // func*
                 llvm::PointerType::getUnqual(ctx)   // env*
             });
  }

  /**
   * Get the raw LLVM FunctionType (for the actual function signature with
   * closure param)
   */
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

/**
 * Raw pointer type - non-owning pointer for C interop (no automatic cleanup)
 * Type annotation: raw_ptr<T> where T is the pointee type
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
 * globals) Type annotation: static_ptr<T> where T is the pointee type Memory
 * safe: never freed, always valid, read-only
 * Represented as a fat pointer struct: { ptr data, i64 length }
 * Can implicitly convert to raw_ptr<T> for function calls (extracts data ptr)
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
  /** Updates the class lifetime args stored by this object. */
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
   * Implemented after ArrayType definition
   */
  inline bool isUnsizedArrayRef() const;

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
   * Implemented after ArrayType definition
   */
  inline llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override;

  /**
   * Get the LLVM type of the referenced value (for load/store operations)
   */
  llvm::Type* getReferencedLLVMType(llvm::LLVMContext& ctx) const {
    return referencedType->toLLVMType(ctx);
  }
};

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
   * Error union is represented as a struct: { i1 isError, <valueType> value }
   * If isError is true, the error code is stored in the value field (as i64)
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

/**
 * Array type: array<T, N> or array<T, M, N> for multi-dimensional.
 * A sized array OWNS its elements inline - [N x T] wherever it lives (a local,
 * a field, a global, an element of another array) - and moves like every
 * other compound value. An unsized array<T> (empty dimensions) is a view of
 * some sized array with the rank erased; it exists only behind `ref` and is
 * carried as the fat struct { ptr data, i32 ndims, ptr dims }.
 */
class ArrayType : public Type {
  TypePtr elementType;             // The element type (e.g., i32)
  std::vector<size_t> dimensions;  // Fixed sizes (e.g., {5} or {3, 2}), empty
                                   // means unsized

 public:
  /** Creates a fixed-size array type with the supplied element type and dimensions. */
  ArrayType(TypePtr elemType, std::vector<size_t> dims)
      : elementType(std::move(elemType)), dimensions(std::move(dims)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Array;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the element type stored by this object. */
  const TypePtr& getElementType() const { return elementType; }
  /** Returns the dimensions stored by this object. */
  const std::vector<size_t>& getDimensions() const { return dimensions; }

  /**
   * Check if this is an unsized array (array<T> without dimensions)
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
   * Used for coercion from array<T, m, n> to array<T> (unsized)
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

/**
 * Module/namespace reference type
 * Used in semantic analysis when accessing module-scoped variables/functions
 * e.g., in "mod_x.mod_y.a", mod_x and mod_x.mod_y have ModuleType
 */
class ModuleType : public Type {
  std::string modulePath;  // e.g., "mod_x" or "$hash$_mod_x"

 public:
  /** Creates a type describing a reference to a module path. */
  explicit ModuleType(std::string path) : modulePath(std::move(path)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Module;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the module path stored by this object. */
  const std::string& getModulePath() const { return modulePath; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "module<" + modulePath + ">"; }

  /**
   * The path as source spells it, without the "$hash$" scope a moon import
   * adds
   */
  std::string toDisplayString() const override {
    return "module<" + displayModulePath(modulePath) + ">";
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (!other.isModule()) return false;
    return modulePath == static_cast<const ModuleType&>(other).modulePath;
  }

  /**
   * Module types don't have LLVM representation - they're resolved at compile
   * time
   */
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

/**
 * Class field information
 */
struct ClassField {
  std::string name;
  TypePtr type;
  size_t index;  // Index in the struct
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;
  DeclarationId declarationId;
};

/**
 * Class method information
 */
struct ClassMethod {
  std::string name;
  std::vector<std::string> typeParameters;  // Generic type params: <T, U>
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;  // Excludes implicit 'this' parameter
  bool isConstructor;               // true if this is the 'init' method
  bool canThrow = false;  // declared with 'throws IError' — may unwind
  bool isUnsafe = false;  // Calls require an unsafe block.
  bool isConst = false;   // `const function`: does not change `this`
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;
  bool isSynthesizedConstructor = false;
  DeclarationId declarationId;
  DeclarationId defaultImplementation;

  /** Reports whether this declaration still has unbound type parameters. */
  bool isGeneric() const { return !typeParameters.empty(); }
};

/** A nominal type's identity within its analysis session. */
class NominalType : public Type {
  friend class TypeRegistry;
  DeclarationId declarationId_;
  std::shared_ptr<const int> declarationSession_;

 protected:
  /** Reports whether both nominal types belong to the same analysis session. */
  bool sameSession(const NominalType& other) const {
    return declarationSession_ == other.declarationSession_;
  }

  /** Reports whether both nominal types refer to the same declaration identity. */
  bool sameDeclaration(const NominalType& other) const {
    if (!declarationId_ || !other.declarationId_)
      logAndThrowError("Nominal equality requires declaration identities");
    return declarationId_ == other.declarationId_ &&
           declarationSession_ == other.declarationSession_;
  }

 public:
  /** Return the declaration identity within this analysis session. */
  DeclarationId getDeclarationId() const { return declarationId_; }
  /** Return the original template, or this declaration for a source type. */
  DeclarationId sourceDeclaration(const DeclarationTable& table) const;
  /** Check that a boundary is using this nominal type's owning session. */
  bool belongsTo(const DeclarationTable& table) const {
    return declarationSession_ == table.session();
  }
};

class InterfaceType;

/**
 * Class type for user-defined classes
 * Classes are represented as LLVM structs with methods as separate functions
 * Generic classes have type parameters (e.g., class List<T>)
 * Specialized classes have type arguments (e.g., List<i32>)
 */
class ClassType : public NominalType {
  friend class TypeRegistry;
  std::string
      name_;  // Source-qualified spelling for diagnostic type signatures.
  std::string
      baseName_;  // User-written base name (e.g., "Unique") for error messages
  sun::semantic_analysis::QualifiedName
      qualifiedName_;  // Structured qualified name for scoping
  std::vector<std::string>
      typeParameters;  // Type params: ["T", "U"] for generic definitions
  std::vector<TypePtr>
      typeArguments;            // Type args: [i32] for specialized classes
  std::string baseGenericName;  // For specialized: original generic class name
  sun::semantic_analysis::QualifiedName
      genericQualifiedName_;  // For specialized: the generic's qualified name
  std::vector<ClassField> fields;
  std::vector<ClassMethod> methods;
  std::unordered_map<DeclarationId, DeclarationId> interfaceImplementations_;
  std::vector<DeclarationId>
      implementedInterfaces;  // Interfaces this class implements
  std::vector<DeclarationId>
      staticOnlyInterfaces;  // Implemented, but not convertible to (see below)
  bool isPacked_ = false;    // "packed class": lay fields out with no padding
  // Lifetime names the class DECLARES ('class Bus<'a>'). Declarations only,
  // never bindings, so sharing one ClassType per class stays sound; the
  // borrow checker uses them to entangle a method's named parameters with
  // the receiver at call sites.
  std::vector<std::string> lifetimeParams_;
  mutable llvm::StructType* cachedLLVMType = nullptr;

 public:
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;

  /** Provides the lifetime parameters associated with this declaration. */
  const std::vector<std::string>& getLifetimeParams() const {
    return lifetimeParams_;
  }
  /** Stores the lifetime parameters associated with this declaration. */
  void setLifetimeParams(std::vector<std::string> names) {
    lifetimeParams_ = std::move(names);
  }

  /** Creates a semantic class descriptor under its declared name. */
  ClassType(std::string className) : name_(std::move(className)) {}

  /**
   * Constructor for generic class definition
   */
  ClassType(std::string className, std::vector<std::string> typeParams)
      : name_(std::move(className)), typeParameters(std::move(typeParams)) {}

  /**
   * Constructor for specialized generic class
   */
  ClassType(std::string name_, std::string baseName,
            std::vector<TypePtr> typeArgs)
      : name_(std::move(name_)),
        typeArguments(std::move(typeArgs)),
        baseGenericName(std::move(baseName)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Class;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }

  /**
   * Base name accessors (user-written name for error messages)
   */
  const std::string& getBaseName() const {
    return baseName_.empty() ? name_ : baseName_;
  }
  /** Sets the unqualified declaration name without changing its enclosing scopes. */
  void setBaseName(std::string bn) { baseName_ = std::move(bn); }
  /** Reports whether the unqualified declaration name is available. */
  bool hasBaseName() const { return !baseName_.empty(); }

  /**
   * Qualified name accessors
   */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return qualifiedName_;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qn) {
    qualifiedName_ = std::move(qn);
  }
  /** Reports whether a name including the enclosing scopes has been assigned. */
  bool hasQualifiedName() const { return !qualifiedName_.baseName.empty(); }

  /**
   * Get user-friendly display name for error messages
   * For specialized classes: "Vec<i32>" or "std.Vec<i32>"
   * For non-specialized classes, preserve the source spelling.
   */
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
      base = name_;
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

  /** Provides the generic parameters declared by this type or function. */
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  /** Provides the concrete types supplied for generic specialization. */
  const std::vector<TypePtr>& getTypeArguments() const { return typeArguments; }
  /** Returns the original generic name used to identify this specialization. */
  const std::string& getBaseGenericName() const { return baseGenericName; }
  /**
   * Qualified name of the generic this specialization was instantiated from
   * (scope path + plain base name), for scope-tree lookups
   */
  const sun::semantic_analysis::QualifiedName& getGenericQualifiedName() const {
    return genericQualifiedName_;
  }
  /** Updates the generic qualified name stored by this object. */
  void setGenericQualifiedName(sun::semantic_analysis::QualifiedName qn) {
    genericQualifiedName_ = std::move(qn);
  }
  /** Reports whether this is a generic declaration rather than a concrete instance. */
  bool isGenericDefinition() const { return !typeParameters.empty(); }
  /** Reports whether this type was instantiated with concrete type arguments. */
  bool isSpecialized() const { return !typeArguments.empty(); }
  /** Provides the field declarations belonging to this type. */
  const std::vector<ClassField>& getFields() const { return fields; }
  /** The selected cleanup method, if this class defines one. */
  DeclarationId deinitializer;

  /** Provides the method declarations belonging to this type. */
  const std::vector<ClassMethod>& getMethods() const { return methods; }

  /** Record the concrete method selected for an interface declaration. */
  void bindInterfaceMethod(DeclarationId requirement,
                           DeclarationId implementation) {
    interfaceImplementations_[requirement] = implementation;
  }

  /** Retrieve the concrete method selected during conformance checking. */
  DeclarationId getInterfaceMethod(DeclarationId requirement) const {
    auto found = interfaceImplementations_.find(requirement);
    if (found == interfaceImplementations_.end())
      logAndThrowError("Interface method implementation has not been resolved");
    return found->second;
  }
  /** Provides the interfaces implemented by this class. */
  const std::vector<DeclarationId>& getImplementedInterfaces() const {
    return implementedInterfaces;
  }

  /** Reports whether this object has field. */
  bool hasField(const std::string& fieldName) const {
    return getField(fieldName) != nullptr;
  }

  /**
   * Returns the new record so callers can set its access info.
   */
  ClassField& addField(const std::string& fieldName, TypePtr fieldType,
                       DeclarationId id = {}) {
    // Caller should check hasField() first and report error with position
    fields.push_back({fieldName, std::move(fieldType), fields.size(),
                      sun::semantic_analysis::Visibility::Private, id});
    return fields.back();
  }

  /** Registers a method signature on this type for lookup and dispatch. */
  ClassMethod& addMethod(const std::string& methodName, TypePtr returnType,
                         std::vector<TypePtr> paramTypes,
                         bool isConstructor = false,
                         std::vector<std::string> typeParams = {},
                         bool canThrow = false) {
    methods.push_back({methodName, std::move(typeParams), std::move(returnType),
                       std::move(paramTypes), isConstructor, canThrow});
    return methods.back();
  }

  /** Record a resolved interface implemented by this class. */
  void addImplementedInterface(const InterfaceType& interface);

  /** Check conformance using the interface's session and declaration ID. */
  bool implementsInterface(const InterfaceType& interface) const;

  /** Mark an implementation whose return ABI prevents dynamic dispatch. */
  void markStaticOnlyInterface(const InterfaceType& interface);

  /** Report whether a resolved interface can be used as a fat pointer. */
  bool convertibleToInterface(const InterfaceType& interface) const;

  /** Retrieve a selected field independently of its spelling and layout. */
  const ClassField* getField(DeclarationId id) const {
    if (!id) return nullptr;
    for (const auto& field : fields)
      if (field.declarationId == id) return &field;
    return nullptr;
  }

  /** Returns the field stored by this object. */
  const ClassField* getField(const std::string& fieldName) const {
    for (const auto& field : fields) {
      if (field.name == fieldName) return &field;
    }
    return nullptr;
  }

  /** Retrieve a selected method without repeating overload resolution. */
  const ClassMethod* getMethod(DeclarationId declaration) const {
    if (!declaration) return nullptr;
    for (const auto& method : methods) {
      if (method.declarationId == declaration) return &method;
    }
    logAndThrowError("Selected method does not belong to this class");
  }

  /** Returns the method stored by this object. */
  const ClassMethod* getMethod(const std::string& methodName) const {
    for (const auto& method : methods) {
      if (method.name == methodName) return &method;
    }
    return nullptr;
  }

  /**
   * Returns true if an array argument of type `from` is compatible with an
   * array parameter of type `to` (same element type, with an unsized parameter
   * accepting any sized array). Mirrors the array coercion allowed elsewhere so
   * that e.g. array<i32, 3, 2> can be passed where array<i32> is expected.
   */
  static bool isArrayCompatible(const TypePtr& from, const TypePtr& to) {
    if (!from || !to || !from->isArray() || !to->isArray()) {
      return false;
    }
    auto* toArr = static_cast<const ArrayType*>(to.get());
    auto* fromArr = static_cast<const ArrayType*>(from.get());
    return toArr->isCompatibleWith(*fromArr);
  }

  /**
   * Returns true if a value of type `from` can be implicitly widened to type
   * `to` (integer-to-wider-integer or float-to-wider-float). This mirrors the
   * numeric widening allowed by free-function overload resolution so that
   * method/constructor overloads accept the same arguments (e.g. an i32 literal
   * passed where an i64 parameter is expected).
   */
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

  /**
   * True if an argument of type `from` reaches an interface-typed parameter
   * `to` by conversion to a fat pointer: an owned class where the interface
   * is taken by value, or a class where `ref Interface` is expected.
   * Mirrors the interface rules of isAssignableTo (type_rules.cpp) so that
   * overload selection accepts what a single known signature accepts.
   * Defined after InterfaceType and typeIsFrameCarrying, which it needs.
   */
  static bool isInterfaceConvertible(const TypePtr& from, const TypePtr& to);

  /**
   * Get method with overload resolution based on argument types.
   * Returns the method whose parameter types best match the provided arg
   * types. An exact match wins; otherwise the first overload reachable by an
   * implicit argument conversion (borrow, array view, numeric or lambda
   * widening, class to interface) is chosen.
   */
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

        // A borrowed scalar can be read into a value parameter, including
        // numeric widening. Borrowed compound values must keep their owner.
        if (argTypes[i]->isReference() &&
            !method.paramTypes[i]->isReference() &&
            typeCopiesByRead(method.paramTypes[i])) {
          TypePtr valueType = unwrapRef(argTypes[i]);
          if (method.paramTypes[i]->equals(*valueType) ||
              isNumericWidenable(valueType, method.paramTypes[i])) {
            continue;
          }
        }

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

        // A class becomes a fat pointer where an interface it implements is
        // expected (issue #219).
        if (isInterfaceConvertible(argTypes[i], method.paramTypes[i])) {
          continue;
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

  /**
   * Get the constructor method (named "init")
   */
  const ClassMethod* getConstructor() const {
    for (const auto& method : methods) {
      if (method.isConstructor) return &method;
    }
    return nullptr;
  }

  /** Returns a readable representation for diagnostics and debugging. */
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
      std::string result = name_ + "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
      }
      result += ">";
      return result;
    }
    return name_;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override { return getDisplayName(); }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* c = dynamic_cast<const ClassType*>(&other)) {
      return sameDeclaration(*c);
    }
    return false;
  }

  /**
   * Classes are value types represented as LLVM structs
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return getStructType(ctx);
  }

  /**
   * Get the actual struct type for the class
   */
  llvm::StructType* getStructType(llvm::LLVMContext& ctx) const {
    if (cachedLLVMType) return cachedLLVMType;

    // LLVM shares layouts by their fields and packing. Source names do not
    // identify layouts: separate declarations may have the same spelling.
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
    cachedLLVMType = llvm::StructType::get(ctx, fieldTypes, isPacked_);
    return cachedLLVMType;
  }

  /**
   * Packed classes have no inter-field padding and struct alignment 1.
   * Must be set before the first getStructType() call, which memoizes.
   */
  bool isPacked() const { return isPacked_; }
  /** Updates the packed stored by this object. */
  void setPacked(bool v) { isPacked_ = v; }

};

/**
 * Interface field information
 */
struct InterfaceField {
  std::string name;
  TypePtr type;
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;
  DeclarationId declarationId;
};

/**
 * Interface method information
 */
struct InterfaceMethod {
  std::string name;
  std::vector<std::string> typeParameters;  // Generic type params: <T, U>
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;  // Excludes implicit 'this' parameter
  bool hasDefaultImpl;    // true if this method has a default implementation
  bool isUnsafe = false;  // Calls require an unsafe block.
  bool isConst = false;   // `const function`: does not change `this`
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;

  DeclarationId declarationId;

  /** Reports whether this declaration still has unbound type parameters. */
  bool isGeneric() const { return !typeParameters.empty(); }
};

// Forward declaration for InterfaceType
class InterfaceType;
/** Shared ownership of a semantic interface description. */
using InterfaceTypePtr = std::shared_ptr<InterfaceType>;

/**
 * Interface type for user-defined interfaces
 * Interfaces define a contract that classes must implement
 */
class InterfaceType : public NominalType {
  friend class TypeRegistry;
  std::string name;       // Fully qualified name (includes library hash)
  std::string baseName_;  // User-written base name for error messages
  std::vector<std::string> typeParameters;  // Generic type params: T, U, etc.
  std::vector<TypePtr>
      typeArguments;  // Type args: [i32] for specialized interfaces
  std::string
      baseGenericName;  // For specialized: original generic interface name
  std::vector<InterfaceField> fields;
  std::vector<InterfaceMethod> methods;
  sun::semantic_analysis::QualifiedName qualifiedName_;
  // Lifetime names the interface DECLARES ('interface ISink<'a>').
  // Declarations only, never bindings - see ClassType::lifetimeParams_.
  std::vector<std::string> lifetimeParams_;

  sun::semantic_analysis::QualifiedName genericQualifiedName_;

 public:
  /** The original template name, independent of specialization and source
   * aliases. */
  const sun::semantic_analysis::QualifiedName& getGenericQualifiedName() const {
    return genericQualifiedName_;
  }
  /** Record the template that produced this type. */
  void setGenericQualifiedName(sun::semantic_analysis::QualifiedName name) {
    genericQualifiedName_ = std::move(name);
  }

  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;

  /** Provides the lifetime parameters associated with this declaration. */
  const std::vector<std::string>& getLifetimeParams() const {
    return lifetimeParams_;
  }
  /** Stores the lifetime parameters associated with this declaration. */
  void setLifetimeParams(std::vector<std::string> names) {
    lifetimeParams_ = std::move(names);
  }

  /** Creates a semantic interface descriptor under its declared name. */
  InterfaceType(std::string interfaceName) : name(std::move(interfaceName)) {}

  /**
   * Source spelling; declaration records carry module ownership.
   */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return qualifiedName_;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qn) {
    qualifiedName_ = std::move(qn);
  }

  /**
   * Constructor for generic interface definition
   */
  InterfaceType(std::string interfaceName, std::vector<std::string> typeParams)
      : name(std::move(interfaceName)), typeParameters(std::move(typeParams)) {}

  /**
   * Constructor for specialized generic interface
   */
  InterfaceType(std::string name_, std::string baseName,
                std::vector<TypePtr> typeArgs)
      : name(std::move(name_)),
        typeArguments(std::move(typeArgs)),
        baseGenericName(std::move(baseName)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Interface;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }

  /**
   * Base name accessors (user-written name for error messages)
   */
  const std::string& getBaseName() const {
    return baseName_.empty() ? name : baseName_;
  }
  /** Sets the unqualified declaration name without changing its enclosing scopes. */
  void setBaseName(std::string bn) { baseName_ = std::move(bn); }
  /** Reports whether the unqualified declaration name is available. */
  bool hasBaseName() const { return !baseName_.empty(); }

  /** Provides the generic parameters declared by this type or function. */
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  /** Provides the concrete types supplied for generic specialization. */
  const std::vector<TypePtr>& getTypeArguments() const { return typeArguments; }
  /** Returns the original generic name used to identify this specialization. */
  const std::string& getBaseGenericName() const { return baseGenericName; }
  /** Reports whether this is a generic declaration rather than a concrete instance. */
  bool isGenericDefinition() const { return !typeParameters.empty(); }
  /** Reports whether this type was instantiated with concrete type arguments. */
  bool isSpecialized() const { return !typeArguments.empty(); }
  /** Provides the field declarations belonging to this type. */
  const std::vector<InterfaceField>& getFields() const { return fields; }
  /** Provides the method declarations belonging to this type. */
  const std::vector<InterfaceMethod>& getMethods() const { return methods; }

  /**
   * Returns the (possibly pre-existing) record so callers can set access.
   */
  InterfaceField& addField(const std::string& fieldName, TypePtr fieldType,
                           DeclarationId id = {}) {
    for (auto& existingField : fields) {
      if (existingField.name == fieldName) return existingField;
    }
    fields.push_back({fieldName, std::move(fieldType),
                      sun::semantic_analysis::Visibility::Private, id});
    return fields.back();
  }

  /** Registers a method signature on this type for lookup and dispatch. */
  InterfaceMethod& addMethod(const std::string& methodName, TypePtr returnType,
                             std::vector<TypePtr> paramTypes,
                             bool hasDefaultImpl = false,
                             std::vector<std::string> typeParams = {}) {
    methods.push_back({methodName, std::move(typeParams), std::move(returnType),
                       std::move(paramTypes), hasDefaultImpl});
    return methods.back();
  }

  /** Returns the field stored by this object. */
  const InterfaceField* getField(const std::string& fieldName) const {
    for (const auto& field : fields) {
      if (field.name == fieldName) return &field;
    }
    return nullptr;
  }

  /** Returns the method stored by this object. */
  const InterfaceMethod* getMethod(const std::string& methodName) const {
    for (const auto& method : methods) {
      if (method.name == methodName) return &method;
    }
    return nullptr;
  }

  /**
   * Rebind one method's return type. Exists for the builtin IError: it is
   * registered before any source is read, so message() starts as
   * static_ptr<u8> and is retargeted to the String class when the stdlib
   * registers one (see SemanticAnalyzer::registerClassShape).
   */
  void setMethodReturnType(const std::string& methodName, TypePtr returnType) {
    for (auto& method : methods) {
      if (method.name == methodName) {
        method.returnType = std::move(returnType);
        return;
      }
    }
  }

  /**
   * Get methods that don't have default implementations (must be implemented by
   * class)
   */
  std::vector<const InterfaceMethod*> getRequiredMethods() const {
    std::vector<const InterfaceMethod*> required;
    for (const auto& method : methods) {
      if (!method.hasDefaultImpl) {
        required.push_back(&method);
      }
    }
    return required;
  }

  /** Returns a readable representation for diagnostics and debugging. */
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

  /** Returns a source-facing type name without internal compiler prefixes. */
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

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* i = dynamic_cast<const InterfaceType*>(&other)) {
      return sameDeclaration(*i);
    }
    return false;
  }

  /**
   * Interfaces are represented as fat pointers: { ptr data, ptr vtable }.
   * The vtable contains method pointers followed by concrete drop glue (or a
   * no-op for a borrow), so an owning interface remains two pointers.
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return getFatPointerType(ctx);
  }

  // ===================================================================
  // Dynamic Dispatch Support (vtable-based polymorphism)
  // ===================================================================

  /**
   * Get the fat pointer struct type for interface values: { ptr data, ptr
   * vtable }
   * - data: pointer to the concrete class instance
   * - vtable: pointer to the implementing class's vtable for this interface
   */
  static llvm::StructType* getFatPointerType(llvm::LLVMContext& ctx) {
    // Check for existing named type to avoid duplicates
    if (auto* existing = llvm::StructType::getTypeByName(
            ctx, sun::semantic_analysis::InterfaceFat)) {
      return existing;
    }
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    return llvm::StructType::create(ctx, {ptrTy, ptrTy},
                                    sun::semantic_analysis::InterfaceFat);
  }

  /**
   * Get the vtable struct type for this interface.
   * Contains one function pointer per method in declaration order.
   * Vtable layout: [method0_ptr, method1_ptr, ...]
   */
  llvm::StructType* getVtableType(llvm::LLVMContext& ctx) const {
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    std::vector<llvm::Type*> slotTypes(methods.size(), ptrTy);
    return llvm::StructType::get(ctx, slotTypes);
  }

  /**
   * Get the slot index for a method in the vtable.
   * Returns -1 if method not found or if the method is generic.
   * Only non-generic methods can be dispatched via vtable.
   */
  int getMethodIndex(DeclarationId declaration) const {
    int index = 0;
    for (const auto& method : methods) {
      if (method.isGeneric()) {
        continue;  // Skip generic methods - they're not in the vtable
      }
      if (method.declarationId == declaration) {
        return index;
      }
      ++index;
    }
    return -1;  // Method not found or is generic
  }

};

inline void ClassType::addImplementedInterface(const InterfaceType& interface) {
  if (!sameSession(interface))
    logAndThrowError(
        "Interface implementation belongs to another analysis session");
  if (!implementsInterface(interface))
    implementedInterfaces.push_back(interface.getDeclarationId());
}

inline bool ClassType::implementsInterface(
    const InterfaceType& interface) const {
  return sameSession(interface) &&
         std::find(implementedInterfaces.begin(), implementedInterfaces.end(),
                   interface.getDeclarationId()) != implementedInterfaces.end();
}

inline void ClassType::markStaticOnlyInterface(const InterfaceType& interface) {
  if (!sameSession(interface))
    logAndThrowError(
        "Interface implementation belongs to another analysis session");
  if (std::find(staticOnlyInterfaces.begin(), staticOnlyInterfaces.end(),
                interface.getDeclarationId()) == staticOnlyInterfaces.end())
    staticOnlyInterfaces.push_back(interface.getDeclarationId());
}

inline bool ClassType::convertibleToInterface(
    const InterfaceType& interface) const {
  return implementsInterface(interface) &&
         std::find(staticOnlyInterfaces.begin(), staticOnlyInterfaces.end(),
                   interface.getDeclarationId()) == staticOnlyInterfaces.end();
}

/**
 * Enum variant information
 */
struct EnumVariant {
  std::string name;
  int64_t value;  // Tag bits; the enum representation determines signedness
  std::vector<TypePtr> payloadTypes;  // empty = unit variant
  DeclarationId declarationId;

  /** Reports whether this object has payload. */
  bool hasPayload() const { return !payloadTypes.empty(); }
};

// Forward declaration for EnumType
class EnumType;
/** Shared ownership of a semantic enum description. */
using EnumTypePtr = std::shared_ptr<EnumType>;

/**
 * Enum type for user-defined enums
 * Enums use an integer representation, with variants as named constants
 * Example: enum Color { Red, Green, Blue }
 */
class EnumType : public NominalType {
  friend class TypeRegistry;
  std::string
      name_;  // Source-qualified spelling for diagnostic type signatures.
  std::string baseName_;     // User-written base name (e.g., "Color")
  std::vector<EnumVariant> variants;
  TypePtr underlyingType_ = std::make_shared<PrimitiveType>(Kind::Int32);
  std::string genericBase_;           // e.g. "Option" for Option_i32
  std::vector<TypePtr> genericArgs_;  // e.g. [i32] for Option_i32
  sun::semantic_analysis::QualifiedName qualifiedName_;

  sun::semantic_analysis::QualifiedName genericQualifiedName_;

 public:
  /** The original template name, independent of specialization and source
   * aliases. */
  const sun::semantic_analysis::QualifiedName& getGenericQualifiedName() const {
    return genericQualifiedName_;
  }
  /** Record the template that produced this type. */
  void setGenericQualifiedName(sun::semantic_analysis::QualifiedName name) {
    genericQualifiedName_ = std::move(name);
  }

  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;

  /**
   * Source spelling; declaration records carry module ownership.
   */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return qualifiedName_;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qn) {
    qualifiedName_ = std::move(qn);
  }

  /** Creates a semantic enum descriptor with its name and optional variants. */
  EnumType(std::string name_, std::string baseName = "")
      : name_(std::move(name_)), baseName_(std::move(baseName)) {}

  /** Creates a semantic enum descriptor with its name and optional variants. */
  EnumType(std::string name_, std::vector<EnumVariant> vars,
           std::string baseName = "")
      : name_(std::move(name_)),
        variants(std::move(vars)),
        baseName_(std::move(baseName)) {}

  /** Return the integer type used to store the enum tag. */
  const TypePtr& getUnderlyingType() const { return underlyingType_; }
  /** Set the integer representation before generating enum storage. */
  void setUnderlyingType(TypePtr type) {
    assert(type && type->isIntegral());
    underlyingType_ = std::move(type);
  }

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Enum;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name_; }
  /** Provides the alternatives declared by this enum. */
  const std::vector<EnumVariant>& getVariants() const { return variants; }

  /**
   * Base name accessor
   */
  const std::string& getBaseName() const {
    return baseName_.empty() ? name_ : baseName_;
  }
  /** Reports whether the unqualified declaration name is available. */
  bool hasBaseName() const { return !baseName_.empty(); }
  /** Sets the unqualified declaration name without changing its enclosing scopes. */
  void setBaseName(std::string baseName) { baseName_ = std::move(baseName); }

  /**
   * Generic specialization origin (e.g. Option_i32 records base "Option" and
   * args [i32]); empty for non-generic enums.
   */
  void setGenericOrigin(std::string base, std::vector<TypePtr> args) {
    genericBase_ = std::move(base);
    genericArgs_ = std::move(args);
  }
  /** Returns the generic base stored by this object. */
  const std::string& getGenericBase() const { return genericBase_; }
  /** Returns the generic args stored by this object. */
  const std::vector<TypePtr>& getGenericArgs() const { return genericArgs_; }
  /** Reports whether this type represents generic specialization values. */
  bool isGenericSpecialization() const { return !genericBase_.empty(); }

  /**
   * Get user-friendly display name for error messages
   */
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
    return name_;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override { return getDisplayName(); }

  /** Register a variant once, retaining its declaration identity. */
  void addVariant(const std::string& variantName, int64_t value,
                  DeclarationId declarationId = {}) {
    for (const auto& v : variants) {
      if (v.name == variantName) return;
    }
    variants.push_back({variantName, value, {}, declarationId});
  }

  /**
   * Attach resolved payload types to a variant (full-analysis phase; payload
   * annotations may reference classes not yet registered during declaration
   * collection).
   */
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

  /**
   * True if any variant carries a payload (tagged-union representation)
   */
  bool hasPayload() const {
    for (const auto& v : variants) {
      if (v.hasPayload()) return true;
    }
    return false;
  }

  /** Returns the variant stored by this object. */
  const EnumVariant* getVariant(const std::string& variantName) const {
    for (const auto& variant : variants) {
      if (variant.name == variantName) return &variant;
    }
    return nullptr;
  }

  /**
   * Check if a variant exists by name
   */
  bool hasVariant(const std::string& variantName) const {
    return getVariant(variantName) != nullptr;
  }

  /**
   * Get the number of variants
   */
  size_t getNumVariants() const { return variants.size(); }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return name_; }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* e = dynamic_cast<const EnumType*>(&other)) {
      return sameDeclaration(*e);
    }
    return false;
  }

  /**
   * Payload-free enums use their integer type. Payload enums need the
   * module DataLayout for storage sizing: LLVMTypeResolver computes the
   * storage struct and caches it here; afterwards toLLVMType serves the
   * cache (e.g. for class field embedding via ClassType::getStructType).
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    if (hasPayload()) {
      if (cachedStorageType) return cachedStorageType;
      logAndThrowError("payload enum '" + getDisplayName() +
                       "' LLVM type requires DataLayout - resolve it via "
                       "LLVMTypeResolver first");
    }
    return underlyingType_->toLLVMType(ctx);
  }

  // LLVM struct caches, populated by LLVMTypeResolver (mirrors
  // ClassType::cachedLLVMType). storage = { i32 tag, [M x unitTy] }; per
  // variant = { i32 tag, T1, T2, ... } GEP'd on the same base pointer.
  mutable llvm::StructType* cachedStorageType = nullptr;
  mutable std::unordered_map<std::string, llvm::StructType*>
      cachedVariantStructs;
};

/**
 * Type factory for common types (singleton pattern)
 */
class Types {
 public:
  /** Returns the shared semantic type descriptor for Sun void values. */
  static TypePtr Void() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Void);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun bool values. */
  static TypePtr Bool() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Bool);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun int8 values. */
  static TypePtr Int8() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Int8);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun int16 values. */
  static TypePtr Int16() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Int16);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun int32 values. */
  static TypePtr Int32() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Int32);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun int64 values. */
  static TypePtr Int64() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Int64);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun uint8 values. */
  static TypePtr UInt8() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::UInt8);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun uint16 values. */
  static TypePtr UInt16() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::UInt16);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun uint32 values. */
  static TypePtr UInt32() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::UInt32);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun uint64 values. */
  static TypePtr UInt64() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::UInt64);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun float32 values. */
  static TypePtr Float32() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Float32);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun float64 values. */
  static TypePtr Float64() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Float64);
    return t;
  }
  /** Returns the shared semantic type descriptor for Sun char values. */
  static TypePtr Char() {
    static auto t = std::make_shared<PrimitiveType>(Type::Kind::Char);
    return t;
  }

  /**
   * Slice type singleton: builtin { i64 start, i64 end } for indexing
   */
  static TypePtr Slice() {
    static auto t = std::make_shared<SliceType>();
    return t;
  }

  /**
   * Module reference type for qualified name resolution (mod_x.mod_y.var)
   */
  static TypePtr Module(const std::string& modulePath) {
    return std::make_shared<ModuleType>(modulePath);
  }

  /**
   * String literals are represented as static_ptr<u8> - immortal, read-only
   * data
   */
  static TypePtr String() { return StaticPointer(UInt8()); }

  /**
   * Create a function-pointer type: function () T
   */
  static TypePtr Function(TypePtr returnType, std::vector<TypePtr> paramTypes,
                          bool canThrow = false, bool requiresUnsafe = false) {
    return std::make_shared<FunctionType>(
        std::move(returnType), std::move(paramTypes), canThrow, requiresUnsafe);
  }

  /**
   * Create a lambda type: () => {} (anonymous function, fat pointer call)
   */
  static TypePtr Lambda(TypePtr returnType, std::vector<TypePtr> paramTypes,
                        bool canThrow = false, bool requiresUnsafe = false) {
    return std::make_shared<LambdaType>(
        std::move(returnType), std::move(paramTypes), canThrow, requiresUnsafe);
  }

  /**
   * Create a raw (non-owning) pointer type: raw_ptr<T> for C interop
   */
  static TypePtr RawPointer(TypePtr pointeeType) {
    return std::make_shared<RawPointerType>(std::move(pointeeType));
  }

  /**
   * Create a static pointer type: static_ptr<T> for immortal data
   * Used for string literals and global constants - memory safe
   */
  static TypePtr StaticPointer(TypePtr pointeeType) {
    return std::make_shared<StaticPointerType>(std::move(pointeeType));
  }

  /**
   * Create a null pointer type (singleton)
   */
  static TypePtr NullPointer() {
    static auto t = std::make_shared<NullPointerType>();
    return t;
  }

  /**
   * Create a reference type: ref(T) with implicit dereferencing
   */
  static TypePtr Reference(TypePtr referencedType, bool isMutable = true) {
    return std::make_shared<ReferenceType>(std::move(referencedType),
                                           isMutable);
  }

  /**
   * Create a fixed-size array type: array<T, N> or array<T, M, N>
   */
  static TypePtr Array(TypePtr elementType, std::vector<size_t> dimensions) {
    return std::make_shared<ArrayType>(std::move(elementType),
                                       std::move(dimensions));
  }

  /** Create a type parameter carrying its optional requirement. */
  static TypePtr TypeParameter(const std::string& name,
                               TypeConstraint constraint = {},
                               DeclarationId declaration = {},
                               std::shared_ptr<const int> session = {}) {
    return std::make_shared<TypeParameterType>(name, std::move(constraint),
                                               TypeProjection::None,
                                               declaration, std::move(session));
  }

  /**
   * Parse a type name string to TypePtr
   */
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

/** The semantic inputs that distinguish instances of one template. */
struct SpecializationKey {
  DeclarationId source;
  DeclarationId enclosing;
  std::vector<TypePtr> arguments;
  std::optional<std::vector<TypePtr>> variadic;

  /** Compare semantic types, including nominal declaration identities. */
  bool operator==(const SpecializationKey& other) const;
};

inline DeclarationId NominalType::sourceDeclaration(
    const DeclarationTable& table) const {
  if (!belongsTo(table))
    logAndThrowError("Nominal type belongs to another analysis session");
  const auto& record = table.get(getDeclarationId());
  return record.specialization ? record.specialization->source
                               : getDeclarationId();
}

/** Bucket instances by template and argument kinds; equality checks structure.
 */
struct SpecializationKeyHash {
  /** Computes a hash for the supplied value for use in unordered containers. */
  size_t operator()(const SpecializationKey& key) const {
    size_t hash = key.source.index();
    auto combine = [&](size_t value) { hash = hash * 31 + value; };
    combine(key.enclosing.index());
    combine(key.arguments.size());
    for (const auto& arg : key.arguments)
      combine(arg ? static_cast<size_t>(arg->getKind()) + 1 : 0);
    combine(key.variadic.has_value());
    if (key.variadic) {
      combine(key.variadic->size());
      for (const auto& arg : *key.variadic)
        combine(arg ? static_cast<size_t>(arg->getKind()) + 1 : 0);
    }
    return hash;
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
  std::unordered_map<DeclarationId, std::shared_ptr<NominalType>> nominalTypes_;

  /** Looks up the class, interface, or enum type for a declaration identity. */
  template <typename T>
  std::shared_ptr<T> nominalType(DeclarationId id, DeclarationKind kind) {
    const auto& record = declarations.get(id);
    if (record.kind != kind)
      logAndThrowError("Declaration kind does not match nominal type");
    auto found = nominalTypes_.find(id);
    if (found != nominalTypes_.end())
      return std::static_pointer_cast<T>(found->second);
    auto type = std::make_shared<T>(record.name);
    type->declarationId_ = id;
    type->declarationSession_ = declarations.session();
    nominalTypes_.emplace(id, type);
    return type;
  }

  std::unordered_map<SpecializationKey, DeclarationId, SpecializationKeyHash>
      specializations_;

 public:
  /** Declaration identities shared by semantic analysis and code generation. */
  DeclarationTable declarations;

  /** The builtin error interface shared throughout this analysis session. */
  std::shared_ptr<InterfaceType> errorInterface;

  /** Initializes the collection of primitive and declared semantic types. */
  TypeRegistry() { registerBuiltins(); }

  /**
   * Register built-in types (IError). The iteration protocol
   * (IIterator/IIterable) lives in stdlib/iterator.sun since it names Option.
   */
  void registerBuiltins() {
    // Create IError interface with code() and message() methods.
    // message() starts as static_ptr<u8> — the only string type that exists
    // before any source is read. When the stdlib's String class is registered,
    // the return type is retargeted to it (an owned clone of the message), so
    // errors can carry text composed at runtime. Without the stdlib, message()
    // stays literal-only.
    auto id = declarations.add(DeclarationKind::Interface, "IError");
    declarations.bindPortable(
        id,
        PortableDeclarationKey::original(
            "4c7b23a9e50e3bb484c7f661e4700d156bac371ed9dd5d23c3d22e2fefc263c1",
            1));
    auto ierror = nominalType<InterfaceType>(id, DeclarationKind::Interface);
    // Not const: every user error class would then have to spell
    // `const function code()`, and errors are caught into plain variables.
    ierror->addMethod("code", Types::Int32(), {}, true).declarationId =
        declarations.add(DeclarationKind::Function, "code", id);
    ierror->addMethod("message", Types::String(), {}, true).declarationId =
        declarations.add(DeclarationKind::Function, "message", id);
    uint64_t ordinal = 2;
    for (const auto& method : ierror->getMethods())
      declarations.bindPortable(
          method.declarationId,
          PortableDeclarationKey::original("4c7b23a9e50e3bb484c7f661e4700d156ba"
                                           "c371ed9dd5d23c3d22e2fefc263c1",
                                           ordinal++));
    errorInterface = ierror;
  }

  /**
   * Check if a type name is a builtin type that cannot be redefined
   * Includes builtin interfaces and type traits used by _is<T>
   */
  bool isBuiltinTypeName(const std::string& name) const {
    static const std::unordered_set<std::string> builtinNames = {
        // Builtin interfaces
        "IError",
        // Type traits for _is<T> intrinsic
        "_Integer", "_Signed", "_Unsigned", "_Float", "_Numeric", "_Primitive"};
    return builtinNames.count(name) > 0;
  }

  /**
   * Non-copyable to prevent accidental duplication
   */
  TypeRegistry(const TypeRegistry&) = delete;
  /** Disallows assignment so ownership and object identity cannot be duplicated. */
  TypeRegistry& operator=(const TypeRegistry&) = delete;

  /**
   * Movable
   */
  TypeRegistry(TypeRegistry&&) = default;
  /** Transfers the stored state from another instance during move assignment. */
  TypeRegistry& operator=(TypeRegistry&&) = default;

  /** Get a source class before its source name has been assigned. */
  std::shared_ptr<ClassType> getClass(DeclarationId id) {
    return nominalType<ClassType>(id, DeclarationKind::Class);
  }

  /** Bind the current source name to a source class's existing identity. */
  std::shared_ptr<ClassType> getClass(DeclarationId id,
                                      const QualifiedName& name) {
    auto type = getClass(id);
    if (!type->getQualifiedName().baseName.empty() &&
        type->getQualifiedName() != name)
      logAndThrowError("Cannot change a nominal type's assigned source name");
    type->name_ = name.lookupName();
    type->setQualifiedName(name);
    type->setBaseName(name.baseName);
    return type;
  }

  /** Get a source interface before its source name has been assigned. */
  std::shared_ptr<InterfaceType> getInterface(DeclarationId id) {
    return nominalType<InterfaceType>(id, DeclarationKind::Interface);
  }

  /** Bind the current source name to a source interface's identity. */
  std::shared_ptr<InterfaceType> getInterface(DeclarationId id,
                                              const QualifiedName& name) {
    auto type = getInterface(id);
    if (!type->getQualifiedName().baseName.empty() &&
        type->getQualifiedName() != name)
      logAndThrowError("Cannot change a nominal type's assigned source name");
    type->name = name.lookupName();
    type->setQualifiedName(name);
    type->setBaseName(name.baseName);
    return type;
  }

  /** Get a source enum before its source name has been assigned. */
  std::shared_ptr<EnumType> getEnum(DeclarationId id) {
    return nominalType<EnumType>(id, DeclarationKind::Enum);
  }

  /** Bind the current source name to a source enum's existing identity. */
  std::shared_ptr<EnumType> getEnum(DeclarationId id,
                                    const QualifiedName& name) {
    auto type = getEnum(id);
    if (!type->getQualifiedName().baseName.empty() &&
        type->getQualifiedName() != name)
      logAndThrowError("Cannot change a nominal type's assigned source name");
    type->name_ = name.lookupName();
    type->setQualifiedName(name);
    type->setBaseName(name.baseName);
    return type;
  }

  /** Intern an instance before resolving its members or body. */
  DeclarationId specialize(const SpecializationKey& key) {
    auto found = specializations_.find(key);
    if (found != specializations_.end()) return found->second;
    const auto& source = declarations.get(key.source);
    auto id = declarations.add(
        source.kind, source.name, key.enclosing ? key.enclosing : source.owner,
        source.module, std::make_shared<const SpecializationKey>(key),
        key.source);
    specializations_.emplace(key, id);
    return id;
  }

  /** Return an existing instance without allocating a declaration. */
  DeclarationId findSpecialization(const SpecializationKey& key) const {
    auto found = specializations_.find(key);
    return found == specializations_.end() ? DeclarationId{} : found->second;
  }

  /** Configure the class attached to an interned specialization. */
  std::shared_ptr<ClassType> getSpecializedClass(
      DeclarationId id, const QualifiedName& name, const QualifiedName& source,
      const std::vector<TypePtr>& arguments) {
    auto type = getClass(id, name);
    type->baseGenericName = source.lookupName();
    type->setBaseName(source.display());
    type->typeArguments = arguments;
    type->setGenericQualifiedName(source);
    return type;
  }

  /** Intern a generic interface template by its source declaration. */
  std::shared_ptr<InterfaceType> getGenericInterface(
      DeclarationId declaration, const QualifiedName& name,
      std::vector<std::string> typeParams) {
    auto type = getInterface(declaration, name);
    type->typeParameters = std::move(typeParams);
    return type;
  }

  /** Configure the interface attached to an interned specialization. */
  std::shared_ptr<InterfaceType> getSpecializedInterface(
      DeclarationId id, const QualifiedName& name, const QualifiedName& source,
      const std::vector<TypePtr>& arguments) {
    auto type = getInterface(id, name);
    type->baseGenericName = source.lookupName();
    type->setBaseName(source.baseName);
    type->typeArguments = arguments;
    type->setGenericQualifiedName(source);
    return type;
  }

  /**
   * Clear all caches (useful for REPL reset)
   */
  void clear() {
    nominalTypes_.clear();
    specializations_.clear();
    errorInterface.reset();
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

/**
 * True if dropping a value of this type must run cleanup code: classes with a
 * deinit method (directly, or transitively through class/enum-typed fields)
 * and payload enums with at least one payload that needs drop.
 */
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

/** Reports whether values of this type require cleanup when their lifetime ends. */
inline bool typeNeedsDrop(const Type* type) {
  std::unordered_set<const Type*> visited;
  return typeNeedsDropImpl(type, visited);
}

/** Reports whether values of this type require cleanup when their lifetime ends. */
inline bool typeNeedsDrop(const TypePtr& type) {
  return typeNeedsDrop(type.get());
}

/**
 * True if a value of this type may carry a lambda environment that lives in
 * a stack frame: a '<'_>' lambda, or anything that can transitively hold
 * one — a class through its fields or generic type arguments (containers
 * hide elements behind raw storage, so the arguments must count), a payload
 * enum, an array. Such a value must not outlive the frame it was built in.
 * References are the sibling case, tracked by the borrow checker's
 * class-stores-refs walk.
 */
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

/** Reports whether this type can retain storage tied to a stack frame. */
inline bool typeIsFrameCarrying(const Type* type) {
  std::unordered_set<const Type*> visited;
  return typeIsFrameCarryingImpl(type, visited);
}

/** Reports whether this type can retain storage tied to a stack frame. */
inline bool typeIsFrameCarrying(const TypePtr& type) {
  return typeIsFrameCarrying(type.get());
}

inline bool ClassType::isInterfaceConvertible(const TypePtr& from,
                                              const TypePtr& to) {
  if (!from || !to) return false;

  // Class -> Interface: the class is owned by the interface value. A borrow
  // never converts this way (it cannot become an owner), and neither does a
  // frame-carrying class (one that can hold a '<'_>' lambda): the interface
  // type would erase the frame binding.
  if (to->isInterface()) {
    if (!from->isClass() || typeIsFrameCarrying(from)) return false;
    auto* iface = static_cast<const InterfaceType*>(to.get());
    return static_cast<const ClassType*>(from.get())
        ->convertibleToInterface(*iface);
  }

  // Class -> ref Interface: the class is borrowed through the fat pointer
  if (to->isReference() && from->isClass()) {
    TypePtr target = unwrapRef(to);
    if (!target || !target->isInterface()) return false;
    auto* iface = static_cast<const InterfaceType*>(target.get());
    return static_cast<const ClassType*>(from.get())
        ->convertibleToInterface(*iface);
  }

  return false;
}

/**
 * A copy of the type with every lifetime NAME stripped, recursively.
 * Lifetime names are relative to one signature's lifetime list; a type that
 * crosses into another namespace - a generic type-parameter binding, whose
 * specialization is shared by every caller - must not carry them along.
 * The <'_> marker itself is identity and stays.
 */
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
    auto result = Types::Lambda(ret, std::move(params), lt->canThrow(),
                                lt->requiresUnsafe());
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

}  // namespace sun::semantic_analysis
