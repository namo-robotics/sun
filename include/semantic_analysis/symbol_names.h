// symbol_names.h — Naming rules for symbols: what a user may call a thing, and
// how an overload set is keyed.

#pragma once

#include <string>
#include <vector>

#include "semantic_analysis/types.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

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

/** Format a function signature for diagnostics and scope inspection. */
std::string formatFunctionSignature(
    const std::string& name,
    const std::vector<sun::semantic_analysis::TypePtr>& paramTypes);

}  // namespace sun::semantic_analysis
