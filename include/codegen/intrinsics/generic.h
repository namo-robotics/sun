// intrinsics/generic.h — Generic intrinsic definitions
//
// Generic intrinsics require a type argument: _sizeof<T>(), _load<T>(ptr, idx)
// These are resolved at compile time based on the type parameter.
//
// The type traits `_is<T>` tests — and the predicate that tests them — live in
// semantic_analysis/type_traits.h, because `<T: _Numeric>` constraints ask the
// same question at a signature and both must answer it identically.

#pragma once

#include "semantic_analysis/type_traits.h"
