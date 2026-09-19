/** Deduces generic arguments from read-only parameter and argument shapes. */
#pragma once
#include <map>

#include "ast/type_annotation.h"
#include "semantic_analysis/type_analysis/type_inferer.h"

/** Pure generic argument deduction with no scope lookup or specialization. */
namespace sun::semantic_analysis::type_analysis {
using sun::types::TypePtr;
/** Deduce ordered type arguments from written parameter shapes. */
InferenceResult<std::vector<TypePtr>> inferTypeArguments(
    const std::vector<std::string>& parameters,
    const std::vector<sun::ast::TypeAnnotation>& shapes,
    const std::vector<TypePtr>& arguments,
    const std::vector<TypePtr>& explicitArguments = {});
/** Deduce ordered type arguments from resolved parameter shapes. */
InferenceResult<std::vector<TypePtr>> inferTypeArguments(
    const std::vector<std::string>& parameters,
    const std::vector<TypePtr>& shapes, const std::vector<TypePtr>& arguments,
    const std::vector<TypePtr>& explicitArguments = {});
/**
 * Match a parameter annotation against the type of the argument it receives,
 * binding any type parameter it names.
 */
void bindTypeParameters(const sun::ast::TypeAnnotation& param,
                        const TypePtr& argType,
                        const std::vector<std::string>& typeParams,
                        std::map<std::string, TypePtr>& bindings);

/**
 * The same for a parameter type that is already resolved, with the type
 * parameters appearing as TypeParameterType.
 */
void bindTypeParameters(const TypePtr& param, const TypePtr& argType,
                        const std::vector<std::string>& typeParams,
                        std::map<std::string, TypePtr>& bindings);

/**
 * True if a type parameter appears anywhere in the type (`T`, `ref T`,
 * `Vec<T>`, ...): the type belongs to a template body, not to a call that
 * can be specialized now.
 */
bool mentionsTypeParameter(const TypePtr& type);

}  // namespace sun::semantic_analysis::type_analysis
