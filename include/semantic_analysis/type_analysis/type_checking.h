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

/**
 * Whether an argument of type `argType` can bind to a `ref` parameter of type
 * `param` without conversion: it is the referenced type itself, a borrow of it
 * whose mutability may convert (only `ref` to `const ref`), or a sized array
 * whose element type matches an unsized `ref array<T>` parameter.
 */
bool referenceParameterAccepts(const sun::types::ReferenceType& param,
                               const sun::types::TypePtr& argType);

/**
 * Whether a raw_ptr argument may be passed as a byte pointer parameter
 * (raw_ptr<i8> or raw_ptr<u8>), like C's void*. Only intrinsic callees allow
 * this, to avoid accidental type erasure in user code.
 */
bool isBytePointerArgument(const sun::types::TypePtr& argType,
                           const sun::types::TypePtr& paramType);

}  // namespace sun::semantic_analysis::type_analysis
