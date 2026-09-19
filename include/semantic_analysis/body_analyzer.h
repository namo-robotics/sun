#pragma once

#include "semantic_analysis/semantic_context.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

class SemanticAnalyzer;

/** Check prepared statements and manage function-body scopes.
 */
class BodyAnalyzer {
 public:
  /** Borrow the shared context and checking helpers. */
  BodyAnalyzer(SemanticContext& context, SemanticAnalyzer& analyzer)
      : ctx_(context), analyzer_(analyzer) {}

  /** Check a prepared block in source order. */
  void analyzeBlock(sun::ast::BlockExprAST& block);

  /** Enter a resolved function's scope and check defaults, parameters, and
   * body. */
  void analyzeFunction(sun::ast::FunctionAST& function);

  /** Bind a lambda's parameters and captures before checking its body. */
  void analyzeLambda(sun::ast::LambdaAST& lambda);

  /** Check a specialized class method under its concrete type bindings. */
  void analyzeMethodWithBindings(
      sun::ast::FunctionAST& method,
      std::shared_ptr<sun::types::ClassType> classType,
      const std::vector<std::string>& typeParams,
      const std::vector<sun::types::TypePtr>& typeArgs);

 private:
  SemanticContext& ctx_;
  SemanticAnalyzer& analyzer_;
};

}  // namespace sun::semantic_analysis
