/** Prepares generic deduction inputs and reports contextual failures. */
#include "semantic_analysis/generic_type_arguments.h"

#include "semantic_analysis/type_analysis/generic_type_arguments.h"
#include "support/error.h"

/** Semantic adapters for generic argument deduction. */
namespace sun::semantic_analysis {
using sun::types::ClassMethod;
using sun::types::TypePtr;

/** Attach the call's name and location to an unsuccessful deduction. */
static std::vector<TypePtr> requireArguments(
    sun::semantic_analysis::type_analysis::InferenceResult<std::vector<TypePtr>>
        result,
    const std::string& what, const std::string& displayName,
    std::optional<sun::support::Position> loc) {
  if (auto* arguments = std::get_if<std::vector<TypePtr>>(&result))
    return std::move(*arguments);
  const auto& failure =
      std::get<sun::semantic_analysis::type_analysis::InferenceFailure>(result);
  sun::support::logAndThrowError(
      "Cannot infer type argument '" + failure.parameter + "' of " + what +
          " '" + displayName +
          "' from the arguments. Give it explicitly, e.g. " + displayName +
          "<i32>(...).",
      loc);
}
std::vector<TypePtr> resolveGenericTypeArguments(
    const GenericFunctionInfo& genericInfo,
    const std::vector<TypePtr>& argTypes, const std::string& displayName,
    std::optional<sun::support::Position> loc,
    const std::vector<TypePtr>& explicitTypeArgs) {
  std::vector<sun::ast::TypeAnnotation> shapes;
  for (const auto& parameter : genericInfo.params)
    shapes.push_back(parameter.second);
  return requireArguments(
      sun::semantic_analysis::type_analysis::inferTypeArguments(
          sun::ast::typeParameterNames(genericInfo.typeParameters), shapes,
          argTypes, explicitTypeArgs),
      "generic function", displayName, loc);
}
std::vector<TypePtr> resolveMethodTypeArguments(
    const ClassMethod& method, const std::vector<TypePtr>& argTypes,
    const std::string& displayName, std::optional<sun::support::Position> loc,
    const std::vector<TypePtr>& explicitTypeArgs) {
  return requireArguments(
      sun::semantic_analysis::type_analysis::inferTypeArguments(
          method.typeParameters, method.paramTypes, argTypes, explicitTypeArgs),
      "generic method", displayName, loc);
}
}  // namespace sun::semantic_analysis
