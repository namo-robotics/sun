/** Prepares already loaded bundle declarations for semantic analysis. */
#include "semantic_analysis/passes/moon_import_preparation_pass.h"

#include "ast/moon_scope_ast.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

void MoonImportPreparationPass::run(const sun::ast::BlockExprAST& block) const {
  for (const auto& expr : block.getBody()) {
    const auto* moon = dynamic_cast<const sun::ast::MoonScopeAST*>(expr.get());
    if (!moon || moon->isOwnBundle()) continue;
    ctx_.results().declarations.importRecords(moon->importedDeclarations);
  }

  // A requirement may refer to any bundle, including one listed later.
  for (const auto& expr : block.getBody()) {
    const auto* moon = dynamic_cast<const sun::ast::MoonScopeAST*>(expr.get());
    if (!moon || moon->isOwnBundle()) continue;
    for (const auto& requirement : moon->requiredDeclarations)
      ctx_.requireDeclaration(requirement.key, moon->getMoonPath(),
                              requirement.expectedKind,
                              requirement.displayName);
  }
}

}  // namespace sun::semantic_analysis::passes
