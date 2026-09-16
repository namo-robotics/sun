#include "semantic_analysis/semantic_pipeline.h"

#include "semantic_analysis/semantic_analyzer.h"

namespace sun {

SemanticPipeline::SemanticPipeline(SemanticAnalyzer& analyzer)
    : context_(analyzer.context()),
      declarationCollectionPass_(context_, analyzer),
      localDeclarationNamingPass_(context_),
      bodyAnalysisPass_(context_, analyzer, localDeclarationNamingPass_) {}

void SemanticPipeline::run(BlockExprAST& block) {
  fieldInitializerPreparationPass_.run(block);
  declarationNamingPass_.run(block, context_.getCurrentScopePath(),
                             context_.currentModulePath(),
                             context_.isAtModuleLevel());
  declarationCollectionPass_.run(block);
  bodyAnalysisPass_.run(block);
}

}  // namespace sun
