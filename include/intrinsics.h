// intrinsics.h — Compiler intrinsic function definitions
//
// Intrinsics are built-in functions that the compiler handles specially.
// They start with underscore and are recognized during parsing/codegen.

#pragma once

#include <string>

namespace sun {

// Intrinsic function identifiers
// Generic intrinsics take a type argument: _sizeof<T>(), _load<T>(ptr, idx)
// Non-generic intrinsics are called like regular functions: _malloc(size)
enum class Intrinsic {
  None,  // Not an intrinsic

  // Generic intrinsics (require type argument)
  Sizeof,         // _sizeof<T>() -> i64
  Init,           // _init<T>(ptr, args...) -> void
  Load,           // _load<T>(ptr, index) -> T
  Store,          // _store<T>(ptr, index, value) -> void
  StaticPtrData,  // _static_ptr_data<T>(static_ptr<T>) -> raw_ptr<T>
  StaticPtrLen,   // _static_ptr_len<T>(static_ptr<T>) -> i64
  PtrAsRaw,       // _ptr_as_raw<T>(ptr<T>) -> raw_ptr<T>
  Is,             // _is<T>(value) -> bool (compile-time type check)

  // Non-generic intrinsics
  LoadI64,   // _load_i64(ptr, index) -> i64
  StoreI64,  // _store_i64(ptr, index, value) -> void
  Malloc,    // _malloc(size) -> raw_ptr<i8>
  Free,      // _free(ptr) -> void

  // Print intrinsics
  PrintI32,      // _print_i32(value) -> void
  PrintI64,      // _print_i64(value) -> void
  PrintF64,      // _print_f64(value) -> void
  PrintNewline,  // _print_newline() -> void
  PrintBytes,    // _print_bytes(ptr, len) -> void
  PrintlnStr,    // _println_str(str) -> void
};

// Convert intrinsic function name to enum
// Returns Intrinsic::None if not recognized
inline Intrinsic getIntrinsic(const std::string& name) {
  // Generic intrinsics
  if (name == "_sizeof") return Intrinsic::Sizeof;
  if (name == "_init") return Intrinsic::Init;
  if (name == "_load") return Intrinsic::Load;
  if (name == "_store") return Intrinsic::Store;
  if (name == "_static_ptr_data") return Intrinsic::StaticPtrData;
  if (name == "_static_ptr_len") return Intrinsic::StaticPtrLen;
  if (name == "_ptr_as_raw") return Intrinsic::PtrAsRaw;
  if (name == "_is") return Intrinsic::Is;

  // Non-generic intrinsics
  if (name == "_load_i64") return Intrinsic::LoadI64;
  if (name == "_store_i64") return Intrinsic::StoreI64;
  if (name == "_malloc") return Intrinsic::Malloc;
  if (name == "_free") return Intrinsic::Free;

  // Print intrinsics
  if (name == "_print_i32") return Intrinsic::PrintI32;
  if (name == "_print_i64") return Intrinsic::PrintI64;
  if (name == "_print_f64") return Intrinsic::PrintF64;
  if (name == "_print_newline") return Intrinsic::PrintNewline;
  if (name == "_print_bytes") return Intrinsic::PrintBytes;
  if (name == "_println_str") return Intrinsic::PrintlnStr;

  return Intrinsic::None;
}

// Check if a name is a generic intrinsic (requires type argument)
inline bool isGenericIntrinsic(Intrinsic i) {
  switch (i) {
    case Intrinsic::Sizeof:
    case Intrinsic::Init:
    case Intrinsic::Load:
    case Intrinsic::Store:
    case Intrinsic::StaticPtrData:
    case Intrinsic::StaticPtrLen:
    case Intrinsic::PtrAsRaw:
    case Intrinsic::Is:
      return true;
    default:
      return false;
  }
}

// Check if a name is any intrinsic
inline bool isIntrinsic(const std::string& name) {
  return getIntrinsic(name) != Intrinsic::None;
}

// Built-in type traits for _is<T> intrinsic
// These are pseudo-interfaces that primitives "implement"
enum class TypeTrait {
  None,       // Not a type trait
  Integer,    // i8, i16, i32, i64, u8, u16, u32, u64
  Signed,     // i8, i16, i32, i64
  Unsigned,   // u8, u16, u32, u64
  Float,      // f32, f64
  Numeric,    // Integer + Float
  Primitive,  // Numeric + bool
};

// Convert type trait name to enum
inline TypeTrait getTypeTrait(const std::string& name) {
  if (name == "_Integer") return TypeTrait::Integer;
  if (name == "_Signed") return TypeTrait::Signed;
  if (name == "_Unsigned") return TypeTrait::Unsigned;
  if (name == "_Float") return TypeTrait::Float;
  if (name == "_Numeric") return TypeTrait::Numeric;
  if (name == "_Primitive") return TypeTrait::Primitive;
  return TypeTrait::None;
}

// Check if a name is a built-in type trait
inline bool isTypeTrait(const std::string& name) {
  return getTypeTrait(name) != TypeTrait::None;
}

}  // namespace sun
