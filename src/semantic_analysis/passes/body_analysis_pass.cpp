#include "semantic_analysis/passes/body_analysis_pass.h"

#include "semantic_analysis/semantic_analyzer.h"

using sun::ast::ExprAST;

/** Provides the ordered passes for semantic analysis. */
namespace sun::semantic_analysis::passes {

void BodyAnalysisPass::run(sun::ast::BlockExprAST& block) const {
  for (auto* root : getSourceRoots(block)) analyzer_.analyzeExpr(*root);
}

std::vector<ExprAST*> BodyAnalysisPass::getSourceRoots(
    const sun::ast::BlockExprAST& block) {
  std::vector<ExprAST*> sourceRoots;
  for (const auto& expr : block.getBody()) {
    const auto* moon = dynamic_cast<const sun::ast::MoonScopeAST*>(expr.get());
    if (moon && !moon->isOwnBundle()) continue;
    sourceRoots.push_back(expr.get());
  }
  return sourceRoots;
}

}  // namespace sun::semantic_analysis::passes
