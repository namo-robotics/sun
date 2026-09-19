/** Defines the common type interface and shared ownership handle. */
#pragma once

#include <memory>
#include <string>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/** Base description shared by all Sun types. */
class Type;
/** Shared ownership of a semantic type description. */
using TypePtr = std::shared_ptr<Type>;

/**
 * Base Type class
 */
class Type {
 public:
  /** Identifies the primitive and composite type categories understood by the
   * compiler. */
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

}  // namespace sun::types
