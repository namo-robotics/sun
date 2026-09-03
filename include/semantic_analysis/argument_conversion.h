#pragma once

// How a call argument reaches its parameter.
//
// Semantic analysis decides this for every argument of every call it accepts
// and records the decisions on the call node (CallExprAST, GenericCallAST).
// Codegen carries each decision out and never compares Sun types at a call
// boundary itself, so the rules live in exactly one place: classifyArgument.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "semantic_analysis/types.h"
#include "support/position.h"

namespace sun {

enum class ArgConversion : uint8_t {
  PassValue,    // the value as it is: scalars, pointers and lambda values
                // (and anything read out of a borrow)
  Move,         // an owning compound by value: the source is invalidated
  Borrow,       // a `ref T` parameter: the argument's address
  ArrayToView,  // a sized array to `ref array<T>`: a view of its storage
                // with the rank erased
  RawPtrAsRef,  // raw_ptr<T> to `ref T`: the pointer is the address
  ClassToInterface,          // an owned class to an owning interface
  BorrowedClassToInterface,  // a borrowed class to an interface parameter:
                             // fat pointer with a no-op drop slot
  ClassToRefInterface,       // a class to `ref Interface`: a fat pointer
                             // spilled to the stack, its address passed
  WidenNumeric,         // a narrower integer or float to a wider parameter
  StaticToRawPtr,       // static_ptr<T> to raw_ptr<T>: its data pointer
  DerefRawPtr,          // raw_ptr<T> to a primitive T: the pointee is loaded
  CVararg,              // past the declared parameters of a C-variadic callee:
                        // C's default argument promotions
};

namespace conversions {

const char* toString(ArgConversion conversion);

// The conversion that hands an argument of `argType` to a parameter of
// `paramType`. `paramType` is null past the declared parameters: a C `...`
// tail when `cVariadicTail`, otherwise a variadic pack. Returns nullopt when
// no lowering exists — acceptance is checked before this (overload
// resolution, isAssignableTo), so a nullopt for an accepted pair means the
// acceptance rules and the lowering rules have drifted apart.
std::optional<ArgConversion> classifyArgument(const TypePtr& argType,
                                              const TypePtr& paramType,
                                              bool cVariadicTail);

// classifyArgument for every argument of a call, in order. Throws the
// compile error for a pair with no lowering.
std::vector<ArgConversion> classifyArguments(
    const std::vector<TypePtr>& argTypes,
    const std::vector<TypePtr>& paramTypes, bool cVariadic,
    const std::string& calleeName, std::optional<Position> loc);

}  // namespace conversions
}  // namespace sun
