#pragma once

/**
 * Prepares generic parameter shapes for sun::semantic_analysis::type_analysis
 * and reports deduction failures using the call's source location and display
 * name. Specialization remains the caller's responsibility.
 */

#include <optional>
#include <string>
#include <vector>

#include "ast/type_annotation.h"
#include "semantic_analysis/semantic_scope.h"
#include "support/position.h"
#include "types/types.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/**
 * Infer `f<...>` for a call written without type arguments, by matching the
 * template's parameter annotations against the argument types. A call may
 * also name only the leading type parameters (`f<i32>(x)` for `f<T, U>`);
 * those are passed as explicitTypeArgs and the rest are inferred. A result
 * may still be a type parameter when the call sits in a template body.
 * Throws a compile error naming the type parameter that could not be bound.
 */
std::vector<sun::types::TypePtr> resolveGenericTypeArguments(
    const sun::semantic_analysis::GenericFunctionInfo& genericInfo,
    const std::vector<sun::types::TypePtr>& argTypes,
    const std::string& displayName, std::optional<sun::support::Position> loc,
    const std::vector<sun::types::TypePtr>& explicitTypeArgs = {});

/**
 * The same for a class method (`obj.m(x)`, `obj.m<i32>(x)`), matching the
 * method record's parameter types, in which the method's own type parameters
 * appear as TypeParameterType.
 */
std::vector<sun::types::TypePtr> resolveMethodTypeArguments(
    const sun::types::ClassMethod& method,
    const std::vector<sun::types::TypePtr>& argTypes,
    const std::string& displayName, std::optional<sun::support::Position> loc,
    const std::vector<sun::types::TypePtr>& explicitTypeArgs = {});

}  // namespace sun::semantic_analysis
