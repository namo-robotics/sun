#pragma once

// Working out a generic's type arguments from the arguments of a call.
//
// Two kinds of template keep their parameters differently: a generic function
// keeps the annotations as written (`x: U`, `v: ref Vec<U>`), while a class
// method's record already holds resolved types with TypeParameterType
// placeholders. Both binders walk the same shapes — the bare parameter, a
// `ref`, a pointer or array element, and the type arguments of a generic
// type — and the first binding of a parameter wins. Interface methods are
// dispatched through the implementing class's method, and lambdas are not
// generic, so these two cover every generic call.
//
// Nothing here needs the analyzer's state; the one step that does — the
// signature a template has under a set of type arguments — is
// SemanticAnalyzer::genericFunctionSignature, defined alongside these in
// src/semantic_analysis/generic_type_arguments.cpp.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ast/type_annotation.h"
#include "support/position.h"
#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/types.h"

namespace sun::generics {

// Infer `f<...>` for a call written without type arguments, by matching the
// template's parameter annotations against the argument types. A call may
// also name only the leading type parameters (`f<i32>(x)` for `f<T, U>`);
// those are passed as explicitTypeArgs and the rest are inferred. A result
// may still be a type parameter when the call sits in a template body.
// Throws a compile error naming the type parameter that could not be bound.
std::vector<TypePtr> inferGenericTypeArguments(
    const GenericFunctionInfo& genericInfo, const std::vector<TypePtr>& argTypes,
    const std::string& displayName, std::optional<Position> loc,
    const std::vector<TypePtr>& explicitTypeArgs = {});

// The same for a class method (`obj.m(x)`, `obj.m<i32>(x)`), matching the
// method record's parameter types, in which the method's own type parameters
// appear as TypeParameterType.
std::vector<TypePtr> inferMethodTypeArguments(
    const ClassMethod& method, const std::vector<TypePtr>& argTypes,
    const std::string& displayName, std::optional<Position> loc,
    const std::vector<TypePtr>& explicitTypeArgs = {});

// Match a parameter annotation against the type of the argument it receives,
// binding any type parameter it names.
void bindTypeParameters(const TypeAnnotation& param, const TypePtr& argType,
                        const std::vector<std::string>& typeParams,
                        std::map<std::string, TypePtr>& bindings);

// The same for a parameter type that is already resolved, with the type
// parameters appearing as TypeParameterType.
void bindTypeParameters(const TypePtr& param, const TypePtr& argType,
                        const std::vector<std::string>& typeParams,
                        std::map<std::string, TypePtr>& bindings);

// True if a type parameter appears anywhere in the type (`T`, `ref T`,
// `Vec<T>`, ...): the type belongs to a template body, not to a call that
// can be specialized now.
bool mentionsTypeParameter(const TypePtr& type);

}  // namespace sun::generics
