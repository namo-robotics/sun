#include "semantic_analysis/passes/import_dependency_validation_pass.h"

#include "ast.h"
#include "semantic_analysis/semantic_context.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

void ImportDependencyValidationPass::run(
    const std::vector<sun::ast::MoonScopeAST*>& imports) const {
  for (const auto* moon : imports)
    for (const auto& requirement : moon->requiredDeclarations)
      context_.requireDeclaration(requirement.key, moon->getMoonPath(),
                                  requirement.expectedKind,
                                  requirement.displayName);
}

}  // namespace sun::semantic_analysis::passes
