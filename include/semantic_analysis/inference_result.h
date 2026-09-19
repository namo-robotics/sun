/** Turns inference failures into source-located semantic diagnostics. */
#pragma once
#include "semantic_analysis/type_analysis/type_inferer.h"
#include "support/error.h"

/** Diagnostic adapters used by semantic analysis. */
namespace sun::semantic_analysis {
/** Return an inferred type or report the caller's contextual diagnostic. */
inline sun::types::TypePtr requireInferredType(
    sun::semantic_analysis::type_analysis::TypeResult result,
    const sun::support::Position& location, const std::string& message) {
  if (auto* type = std::get_if<sun::types::TypePtr>(&result)) return *type;
  sun::support::logAndThrowError(message, location);
}
}  // namespace sun::semantic_analysis
