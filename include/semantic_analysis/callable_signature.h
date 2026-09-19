#pragma once

#include <functional>
#include <string>
#include <vector>

#include "types/types.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Identify an overload by its source name and exact semantic parameter types.
 */
struct CallableSignature {
  std::string name;
  std::vector<sun::types::TypePtr> parameters;

  /** Compare types structurally, including nominal declaration identities. */
  bool operator==(const CallableSignature& other) const {
    return name == other.name &&
           sun::types::sameTypeArguments(parameters, other.parameters);
  }
};

/** Hash stable signature properties; equality checks the complete type
 * structure. */
struct CallableSignatureHash {
  /** Keep the hash independent of mutable display names and type annotations.
   */
  size_t operator()(const CallableSignature& signature) const {
    size_t hash = std::hash<std::string>{}(signature.name);
    hash ^=
        signature.parameters.size() + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    for (const auto& type : signature.parameters)
      hash ^= (type ? static_cast<size_t>(type->getKind()) + 1 : 0) +
              0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

}  // namespace sun::semantic_analysis
