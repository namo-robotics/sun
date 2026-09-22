#pragma once

#include <vector>

#include "ast/ast_fwd.h"

/** Resolves declarations and checks the meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticAnalyzer;

/** Provides the ordered passes for semantic analysis. */
namespace passes {

/** Check source bodies after imported bundles have been prepared. */
class BodyAnalysisPass {
 public:
  /** Borrow the analysis session's analyzer. */
  explicit BodyAnalysisPass(SemanticAnalyzer& analyzer) : analyzer_(analyzer) {}

  /** Check source bodies after imported bundles have been prepared. */
  void run(sun::ast::BlockExprAST& block) const;

 private:
  /** Borrow source roots, including the bundle being built, excluding imports.
   */
  static std::vector<sun::ast::ExprAST*> getSourceRoots(
      const sun::ast::BlockExprAST& block);

  SemanticAnalyzer& analyzer_;
};

}  // namespace passes
}  // namespace sun::semantic_analysis
