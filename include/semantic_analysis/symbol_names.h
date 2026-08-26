// symbol_names.h — Naming rules for symbols: what a user may call a thing, and
// how an overload set is keyed.

#pragma once

#include <string>
#include <vector>

#include "semantic_analysis/types.h"

namespace sun::names {

/**
 * True for a name starting with '_', which is reserved for builtins. User code
 * may not declare one.
 */
bool isReservedIdentifier(const std::string& name);

/**
 * True for an intrinsic function name. Intrinsics live in the same reserved
 * '_' namespace, so this is the same test read the other way round: a call to
 * a '_' name is a call to the compiler, not to a declared function.
 */
inline bool isIntrinsic(const std::string& name) {
  return !name.empty() && name[0] == '_';
}

/**
 * The key an overload is registered under: "name(type1,type2,...)". Two
 * functions with the same name and different parameter types get different
 * keys, which is what makes overloading work.
 */
std::string getFunctionSignature(const std::string& name,
                                 const std::vector<sun::TypePtr>& paramTypes);

}  // namespace sun::names
