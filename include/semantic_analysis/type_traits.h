// type_traits.h — Built-in type traits, and the one predicate that tests them
//
// A trait is a pseudo-interface that types "implement" without declaring it:
// `_Numeric` covers every number, `_Lambda` covers every closure type. The same
// vocabulary is used in two places, and `satisfies()` is what keeps them from
// drifting apart:
//
//   _is<_Numeric>(x)              — asked in a function body, about a value
//   function f<T: _Numeric>(...)  — asked at a signature, about a type argument
//
// Beyond the built-in traits, a name here may be an interface the type
// implements, or the type's own name for an exact match.

#pragma once

#include <string>

#include "semantic_analysis/types.h"

namespace sun {

// The built-in traits. Every one of these is a set of types, not a declared
// interface, so a primitive can satisfy it without implementing anything.
enum class TypeTrait {
  None,       // Not a built-in trait — an interface or a type name
  Integer,    // i8, i16, i32, i64, u8, u16, u32, u64
  Signed,     // i8, i16, i32, i64
  Unsigned,   // u8, u16, u32, u64
  Float,      // f32, f64
  Numeric,    // Integer + Float
  Primitive,  // Numeric + bool
  Lambda,     // _Lambda: any closure type, however it was written
  Function,   // _Function: a named-function value (function (Args) Result)
  Callable,   // _Callable: Lambda + Function — anything that can be called
};

// Look up a trait by the name written in source. Returns None when the name is
// not a built-in trait, which is not an error: it may still be an interface or
// a concrete type name.
inline TypeTrait getTypeTrait(const std::string& name) {
  if (name == "_Integer") return TypeTrait::Integer;
  if (name == "_Signed") return TypeTrait::Signed;
  if (name == "_Unsigned") return TypeTrait::Unsigned;
  if (name == "_Float") return TypeTrait::Float;
  if (name == "_Numeric") return TypeTrait::Numeric;
  if (name == "_Primitive") return TypeTrait::Primitive;
  if (name == "_Lambda") return TypeTrait::Lambda;
  if (name == "_Function") return TypeTrait::Function;
  if (name == "_Callable") return TypeTrait::Callable;
  return TypeTrait::None;
}

// True when the name is one of the built-in traits above.
inline bool isTypeTrait(const std::string& name) {
  return getTypeTrait(name) != TypeTrait::None;
}

namespace traits {

/**
 * Does `type` satisfy `name`? The name is a built-in trait, an interface the
 * type implements, or a type name to match exactly. A `ref T` is unwrapped
 * first, so a borrow satisfies whatever its referent does.
 *
 * A null type satisfies nothing.
 */
bool satisfies(const TypePtr& type, const std::string& name);

}  // namespace traits
}  // namespace sun
