#include "semantic_analysis/semantic_pipeline.h"

#include "semantic_analysis/declaration_identity_pass.h"
#include "semantic_analysis/semantic_analyzer.h"

using sun::ast::ExprAST;

namespace sun::semantic_analysis {

SemanticPipeline::SemanticPipeline(
    sun::semantic_analysis::SemanticAnalyzer& analyzer)
    : analyzer_(analyzer),
      context_(analyzer.context()),
      declarationCollectionPass_(context_, analyzer) {}

void SemanticPipeline::run(sun::ast::BlockExprAST& block,
                           const std::function<void()>& declarationsReady) {
  fieldInitializerPreparationPass_.run(block);
  DeclarationIdentityPass(context_.types()->declarations).run(block);
  if (declarationsReady) declarationsReady();
  declarationNamingPass_.run(block, context_.getCurrentScopePath(),
                             context_.isAtModuleLevel());
  declarationCollectionPass_.run(block);
  analyzer_.bodies().analyzeBlock(block);
}

void SemanticPipeline::prepareGenerated(const ExprAST& expression,
                                        DeclarationId owner,
                                        const ExprAST* origin) {
  auto& table = context_.types()->declarations;
  DeclarationIdentityPass(table).run(
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
