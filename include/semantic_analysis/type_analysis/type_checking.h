/** Type compatibility checks that inspect supplied type descriptions only. */
#pragma once
#include <cstdint>

#include "types/argument_compatibility.h"
#include "types/types.h"

/** Pure type inference and compatibility rules used by semantic analysis. */
namespace sun::semantic_analysis::type_analysis {
// The compatibility rules live with the types; they are re-exported here so
// analysis code can keep naming them alongside the other type checks.
using sun::types::argumentAccepts;
using sun::types::isAssignableTo;
using sun::types::isBytePointerArgument;
using sun::types::referenceParameterAccepts;

/**
 * True when an integer literal, given as a magnitude and a sign, is
 * representable in the integer or bool primitive `kind`. The magnitude spans
 * the whole u64 range, so 18446744073709551615 fits u64 while anything above
 * i64's maximum does not fit i64.
 */
bool literalFitsInType(uint64_t magnitude, bool negative,
                       sun::types::Type::Kind kind);

/**
 * The type a variable of `original` type has once an `_is<T>` guard has
 * shown it to be `narrowed`: the more specific of the two, where a class
 * beats an interface it implements and anything concrete beats a type
 * parameter. Null when the two do not relate, so no narrowing applies.
 */
sun::types::TypePtr moreSpecificNarrowing(const sun::types::TypePtr& original,
                                          const sun::types::TypePtr& narrowed);

}  // namespace sun::semantic_analysis::type_analysis
