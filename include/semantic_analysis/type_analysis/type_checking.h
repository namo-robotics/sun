/** Type compatibility checks that inspect supplied type descriptions only. */
#pragma once
#include <cstdint>

#include "types/types.h"

/** Pure type inference and compatibility rules used by semantic analysis. */
namespace sun::semantic_analysis::type_analysis {
/**
 * True when a value of type `from` may be used where `to` is expected.
 * Covers exact equality, integer widening, f32/f64 conversion, static_ptr to
 * raw_ptr narrowing, class-to-interface conformance (through a `ref` on either
 * side), a non-throwing lambda where a throwing one is expected, and reading a
 * scalar out of a borrow. A compound read out of a borrow is rejected: that
 * would give the copy and the borrowed value the same buffer.
 */
bool isAssignableTo(const sun::types::TypePtr& from,
                    const sun::types::TypePtr& to);

/**
 * True when an integer literal, given as a magnitude and a sign, is
 * representable in the integer or bool primitive `kind`. The magnitude spans
 * the whole u64 range, so 18446744073709551615 fits u64 while anything above
 * i64's maximum does not fit i64.
 */
bool literalFitsInType(uint64_t magnitude, bool negative,
                       sun::types::Type::Kind kind);

}  // namespace sun::semantic_analysis::type_analysis
