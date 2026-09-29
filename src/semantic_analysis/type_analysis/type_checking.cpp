/**
 * Checks compatibility using supplied, read-only type descriptions.
 * Keep these predicates independent of semantic sessions, expression ASTs,
 * and diagnostics. Contextual coercion and error reporting belong in
 * type_rules.
 */
#include "semantic_analysis/type_analysis/type_checking.h"

/** Pure type inference and compatibility rules used by semantic analysis. */
namespace sun::semantic_analysis::type_analysis {
using sun::types::Type;
using sun::types::TypePtr;

/** Reports whether an integer magnitude and sign fit the target numeric type.
 */
bool literalFitsInType(uint64_t magnitude, bool negative,
                       sun::types::Type::Kind kind) {
  // A signed type of `bits` width holds -2^(bits-1) .. 2^(bits-1)-1; the
  // negative side reaches one further than the positive side.
  auto fitsSigned = [&](int bits) {
    const uint64_t half = uint64_t(1) << (bits - 1);
    return negative ? magnitude <= half : magnitude < half;
  };
  auto fitsUnsigned = [&](uint64_t max) {
    return !negative && magnitude <= max;
  };
  switch (kind) {
    case sun::types::Type::Kind::Int8:
      return fitsSigned(8);
    case sun::types::Type::Kind::Int16:
      return fitsSigned(16);
    case sun::types::Type::Kind::Int32:
      return fitsSigned(32);
    case sun::types::Type::Kind::Int64:
      return fitsSigned(64);
    case sun::types::Type::Kind::UInt8:
      return fitsUnsigned(UINT8_MAX);
    case sun::types::Type::Kind::UInt16:
      return fitsUnsigned(UINT16_MAX);
    case sun::types::Type::Kind::UInt32:
      return fitsUnsigned(UINT32_MAX);
    case sun::types::Type::Kind::UInt64:
      return fitsUnsigned(UINT64_MAX);
    case sun::types::Type::Kind::Bool:
      return fitsUnsigned(1);
    default:
      return false;
  }
}

/** Orders types by specificity: class, then interface, then type parameter. */
TypePtr moreSpecificNarrowing(const TypePtr& original,
                              const TypePtr& narrowed) {
  if (!original || original->isTypeParameter()) return narrowed;
  if (original->isInterface() && narrowed->isClass()) {
    const auto& cls = static_cast<const sun::types::ClassType&>(*narrowed);
    const auto& iface =
        static_cast<const sun::types::InterfaceType&>(*original);
    if (cls.implementsInterface(iface)) return narrowed;
  }
  if (original->isClass() && narrowed->isInterface()) {
    const auto& cls = static_cast<const sun::types::ClassType&>(*original);
    const auto& iface =
        static_cast<const sun::types::InterfaceType&>(*narrowed);
    if (cls.implementsInterface(iface)) return original;
  }
  // An interface never narrows to another interface (no interface inheritance)
  return nullptr;
}

}  // namespace sun::semantic_analysis::type_analysis
