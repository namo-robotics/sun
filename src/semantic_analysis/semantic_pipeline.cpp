#include "semantic_analysis/semantic_pipeline.h"

#include "semantic_analysis/passes/declaration_identity_pass.h"
#include "semantic_analysis/passes/declaration_preparation_guard.h"
#include "semantic_analysis/semantic_analyzer.h"

using sun::ast::ExprAST;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

SemanticPipeline::SemanticPipeline(
    sun::semantic_analysis::SemanticAnalyzer& analyzer)
    : analyzer_(analyzer),
      context_(analyzer.context()),
      importRecordRegistrationPass_(context_.results().declarations),
      importDependencyValidationPass_(context_),
      importCompletionPass_(analyzer),
      declarationIdentityPass_(context_.results().declarations),
      globalRegistrationPass_(context_.results().declarations),
      typeRegistrationPass_(context_),
      declarationCollectionPass_(context_, analyzer),
      bodyAnalysisPass_(analyzer),
      globalInitializerEvaluationPass_(context_.results().declarations) {}

void SemanticPipeline::run(sun::ast::BlockExprAST& block,
                           const std::function<void()>& declarationsReady) {
  prepareImports(block);
  // Imported roots are complete. Keep subsequent source preparation and
  // body analysis from revisiting their declarations.
  fieldInitializerPreparationPass_.run(block, /*skipImportedMoons=*/true);
  declarationIdentityPass_.run(block, /*skipImportedMoons=*/true,
                               declarationsReady);
  declarationNamingPass_.run(block, context_.getCurrentScopePath(),
                             context_.isAtModuleLevel(),
                             /*skipImportedMoons=*/true);
  globalRegistrationPass_.run(block);
  typeRegistrationPass_.run(block);
  declarationCollectionPass_.run(block);
  bodyAnalysisPass_.run(block);
  globalInitializerEvaluationPass_.run(block);
}

std::vector<sun::ast::MoonScopeAST*> SemanticPipeline::getMoonImports(
    const sun::ast::BlockExprAST& block) {
  std::vector<sun::ast::MoonScopeAST*> imports;
  for (const auto& expr : block.getBody()) {
    auto* moon = dynamic_cast<sun::ast::MoonScopeAST*>(expr.get());
    if (!moon || moon->isOwnBundle()) continue;
    imports.push_back(moon);
  }
  return imports;
}

void SemanticPipeline::prepareImports(sun::ast::BlockExprAST& block) {
  const auto imports = getMoonImports(block);
  importRecordRegistrationPass_.run(imports);
  importDependencyValidationPass_.run(imports);
  fieldInitializerPreparationPass_.run(imports);
  declarationIdentityPass_.run(imports);
  declarationNamingPass_.run(imports, context_.getCurrentScopePath());
  typeRegistrationPass_.run(imports);

  // A consumer bundle can instantiate a dependency's template without adding
  // that specialization to the dependency's metadata. Reconstructed bodies
  // must wait until every imported bundle's declarations are ready.
  passes::DeclarationPreparationGuard preparation(analyzer_.generics());
  declarationCollectionPass_.run(imports);
  importCompletionPass_.run(imports);
  preparation.complete();
}

void SemanticPipeline::prepareGenerated(const ExprAST& expression,
                                        DeclarationId owner,
                                        const ExprAST* origin) {
  auto& table = context_.results().declarations;
  declarationIdentityPass_.run(
      expression, owner, owner ? table.get(owner).module : DeclarationId{},
      origin);
}

void SemanticPipeline::prepareGenerated(ExprAST& expression,
                                        const std::vector<std::string>& scope,
                                        DeclarationId owner,
                                        const ExprAST* origin) {
  prepareGenerated(expression, owner, origin);
  declarationNamingPass_.run(expression, scope, false);
}

}  // namespace sun::semantic_analysis
