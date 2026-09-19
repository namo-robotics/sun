// symbol_names.cpp — Naming rules for symbols (see symbol_names.h)

#include "semantic_analysis/symbol_names.h"

#include <unordered_set>

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/**
 * Reserved identifiers are for builtins only (e.g. _is&lt;T&gt;, _sizeof&lt;T&gt;).
 * The exception is the dunder methods a class implements to overload an
 * operator: user code has to be able to spell those.
 */
bool isReservedIdentifier(const std::string& name) {
  if (name.empty() || name[0] != '_') return false;
  static const std::unordered_set<std::string> allowedDunders = {
      "__index__",     // obj[i] read
      "__setindex__",  // obj[i] = val write
      "__slice__",     // obj[a:b] slicing
  };
  if (allowedDunders.count(name)) return false;
  return true;
}

/** Formats a readable function signature for compiler diagnostics. */
std::string formatFunctionSignature(
    const std::string& name,
    const std::vector<sun::semantic_analysis::TypePtr>& paramTypes) {
  std::string sig = name + "(";
  for (size_t i = 0; i < paramTypes.size(); ++i) {
    if (i > 0) sig += ",";
    sig += paramTypes[i] ? paramTypes[i]->toString() : "?";
  }
  sig += ")";
  return sig;
}

}  // namespace sun::semantic_analysis
