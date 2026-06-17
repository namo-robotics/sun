// intrinsics/generic.h — Generic intrinsic definitions
//
// Generic intrinsics require a type argument: _sizeof<T>(), _load<T>(ptr, idx)
// These are resolved at compile time based on the type parameter.

#pragma once

#include <string>

namespace sun {

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
