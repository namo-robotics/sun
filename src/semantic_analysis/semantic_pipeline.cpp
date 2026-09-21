#include "semantic_analysis/semantic_pipeline.h"

#include "semantic_analysis/constants/constant_evaluator.h"
#include "semantic_analysis/passes/declaration_identity_pass.h"
#include "semantic_analysis/semantic_analyzer.h"

using sun::ast::ExprAST;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

SemanticPipeline::SemanticPipeline(
    sun::semantic_analysis::SemanticAnalyzer& analyzer)
    : analyzer_(analyzer),
      context_(analyzer.context()),
      declarationCollectionPass_(context_, analyzer) {}

void SemanticPipeline::run(sun::ast::BlockExprAST& block,
                           const std::function<void()>& declarationsReady) {
  fieldInitializerPreparationPass_.run(block);
  passes::DeclarationIdentityPass(context_.results().declarations).run(block);
  if (declarationsReady) declarationsReady();
  declarationNamingPass_.run(block, context_.getCurrentScopePath(),
                             context_.isAtModuleLevel());
  declarationCollectionPass_.run(block);
  analyzer_.bodies().analyzeBlock(block);
  // Every expression now has its type and every name its declaration, which
  // is what evaluating the file-scope initializers needs.
  constants::ConstantEvaluator(context_.results().declarations)
      .evaluateGlobals(block);
}

void SemanticPipeline::prepareGenerated(const ExprAST& expression,
                                        DeclarationId owner,
                                        const ExprAST* origin) {
  auto& table = context_.results().declarations;
  passes::DeclarationIdentityPass(table).run(
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
