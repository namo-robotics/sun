// visibility.h — Access-modifier model shared by the parser, semantic analyzer,
// moon builder and tooling.
//
// Sun is private by default: `public` is the only modifier. Privacy is
// module-scoped: an item owned by module M is reachable from code whose
// enclosing module is M or a descendant of M. Class members are owned by the
// module that defines the class. Contents of an imported `.moon` live under a
// `$hash$` scope segment, so importer code is never a descendant of a bundle
// module and bundle-private items remain hidden through their declaration
// ownership.

#pragma once

#include <cstdint>

#include "semantic_analysis/module_path.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** The access level used to control declaration lookup across scopes. */
enum class Visibility : uint8_t { Private = 0, Public = 1 };

/** Returns the source keyword corresponding to an access level. */
inline const char* visibilityKeyword(Visibility v) {
  return v == Visibility::Public ? "public" : "private";
}

}  // namespace sun::semantic_analysis
