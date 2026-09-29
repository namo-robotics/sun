// argument_compatibility.h — Which values may stand in for which types
//
// Two questions, answered from the types alone: may a value of one type be
// assigned where another is expected, and may an argument be passed to a
// parameter. Every place that picks or checks a callee (free-function overload
// lookup, class method and constructor lookup, and the check on a resolved
// call) asks the same `argumentAccepts`, so a call is accepted or rejected
// the same way however its callee was found.

#pragma once

#include "types/types.h"

/** Type definitions shared by analysis and code generation. */
namespace sun::types {

/**
 * True when a value of type `from` may be used where `to` is expected.
 * Covers exact equality, integer widening, f32/f64 conversion, static_ptr to
 * raw_ptr narrowing, class-to-interface conformance (through a `ref` on either
 * side), a non-throwing lambda where a throwing one is expected, and reading a
 * scalar out of a borrow. A compound read out of a borrow is rejected: that
 * would give the copy and the borrowed value the same buffer.
 */
bool isAssignableTo(const TypePtr& from, const TypePtr& to);

/**
 * Whether an argument of type `argType` can bind to a `ref` parameter of type
 * `param` without conversion: it is the referenced type itself, a borrow of it
 * whose mutability may convert (only `ref` to `const ref`), or a sized array
 * whose element type matches an unsized `ref array<T>` parameter.
 */
bool referenceParameterAccepts(const ReferenceType& param,
                               const TypePtr& argType);

/**
 * Whether a raw_ptr argument may be passed as a byte pointer parameter
 * (raw_ptr<i8> or raw_ptr<u8>), like C's void*. Only intrinsic callees allow
 * this, to avoid accidental type erasure in user code.
 */
bool isBytePointerArgument(const TypePtr& argType, const TypePtr& paramType);

/**
 * Whether an argument of type `argType` may be passed to a parameter of type
 * `paramType`. On top of isAssignableTo, a call site also accepts: the
 * referent or a compatible borrow for a `ref` parameter, a raw_ptr<T> where
 * `ref T` or a primitive T is expected (read through the pointer), and null
 * for any pointer. With `allowBytePointer`, any raw_ptr passes as a byte
 * pointer; only intrinsics ask for that. Both types must be non-null.
 */
bool argumentAccepts(const TypePtr& argType, const TypePtr& paramType,
                     bool allowBytePointer);

}  // namespace sun::types
