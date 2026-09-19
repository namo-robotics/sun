/** Constructs common primitive and structural type descriptions. */
#pragma once

#include "types/array_type.h"
#include "types/function_type.h"
#include "types/lambda_type.h"
#include "types/module_type.h"
#include "types/pointer_types.h"
#include "types/primitive_type.h"
#include "types/reference_type.h"
#include "types/slice_type.h"
#include "types/type_parameter_type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
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
   * Create a raw (non-owning) pointer type: raw_ptr&lt;T&gt; for C interop
   */
  static TypePtr RawPointer(TypePtr pointeeType) {
    return std::make_shared<RawPointerType>(std::move(pointeeType));
  }

  /**
   * Create a static pointer type: static_ptr&lt;T&gt; for immortal data
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

}  // namespace sun::types
