#include "semantic_analysis/semantic_pipeline.h"

#include "semantic_analysis/declaration_identity_pass.h"
#include "semantic_analysis/semantic_analyzer.h"

namespace sun {

SemanticPipeline::SemanticPipeline(SemanticAnalyzer& analyzer)
    : analyzer_(analyzer),
      context_(analyzer.context()),
      declarationCollectionPass_(context_, analyzer) {}

void SemanticPipeline::run(BlockExprAST& block) {
  fieldInitializerPreparationPass_.run(block);
  DeclarationIdentityPass(context_.types()->declarations).run(block);
  declarationNamingPass_.run(block, context_.getCurrentScopePath(),
                             context_.currentModulePath(),
                             context_.isAtModuleLevel());
  declarationCollectionPass_.run(block);
  analyzer_.bodies().analyzeBlock(block);
}

void SemanticPipeline::prepareGenerated(const ExprAST& expression,
                                        DeclarationId owner) {
  auto& table = context_.types()->declarations;
  DeclarationIdentityPass(table).run(
      expression, owner, owner ? table.get(owner).module : DeclarationId{});
}

void SemanticPipeline::prepareGenerated(ExprAST& expression,
                                        const std::vector<std::string>& scope,
                                        const std::vector<std::string>& module,
                                        DeclarationId owner) {
  prepareGenerated(expression, owner);
  declarationNamingPass_.run(expression, scope, module, false);
}

}  // namespace sun
