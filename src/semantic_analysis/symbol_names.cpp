// symbol_names.cpp — Naming rules for symbols (see symbol_names.h)

#include "semantic_analysis/symbol_names.h"

#include <unordered_set>

namespace sun::names {

// Reserved identifiers are for builtins only (e.g. _is<T>, _sizeof<T>).
// The exception is the dunder methods a class implements to overload an
// operator: user code has to be able to spell those.
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

std::string getFunctionSignature(const std::string& name,
                                 const std::vector<sun::TypePtr>& paramTypes) {
  std::string sig = name + "(";
  for (size_t i = 0; i < paramTypes.size(); ++i) {
    if (i > 0) sig += ",";
    sig += paramTypes[i] ? paramTypes[i]->toString() : "?";
  }
  sig += ")";
  return sig;
}

}  // namespace sun::names
