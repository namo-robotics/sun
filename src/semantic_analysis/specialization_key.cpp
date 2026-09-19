/** Compares the semantic inputs to nominal specializations. */
#include "semantic_analysis/type_registry.h"

/** Registers declarations and their nominal types during semantic analysis. */
namespace sun::semantic_analysis {
using sun::types::sameTypeArguments;

bool SpecializationKey::operator==(const SpecializationKey& other) const {
  return source == other.source && enclosing == other.enclosing &&
         sameTypeArguments(arguments, other.arguments) &&
         variadic.has_value() == other.variadic.has_value() &&
         (!variadic || sameTypeArguments(*variadic, *other.variadic));
}

}  // namespace sun::semantic_analysis
