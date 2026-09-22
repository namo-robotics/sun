#include "semantic_analysis/passes/global_initializer_evaluation_pass.h"

#include "semantic_analysis/constants/constant_evaluator.h"

/** Provides the ordered passes for semantic analysis. */
namespace sun::semantic_analysis::passes {

void GlobalInitializerEvaluationPass::run(
    const sun::ast::BlockExprAST& block) const {
  constants::ConstantEvaluator(declarations_).evaluateGlobals(block);
}

}  // namespace sun::semantic_analysis::passes
