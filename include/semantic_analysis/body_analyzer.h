#pragma once

#include "semantic_analysis/semantic_context.h"

class SemanticAnalyzer;

/** Check prepared statements and manage function-body scopes.
 */
class BodyAnalyzer {
 public:
  /** Borrow the shared context and checking helpers. */
  BodyAnalyzer(SemanticContext& context, SemanticAnalyzer& analyzer)
      : ctx_(context), analyzer_(analyzer) {}

  /** Check a prepared block in source order. */
  void analyzeBlock(BlockExprAST& block);

  /** Enter a resolved function's scope and check defaults, parameters, and
   * body. */
  void analyzeFunction(FunctionAST& function);

  /** Bind a lambda's parameters and captures before checking its body. */
  void analyzeLambda(LambdaAST& lambda);

  /** Check a specialized class method under its concrete type bindings. */
  void analyzeMethodWithBindings(FunctionAST& method,
                                 std::shared_ptr<sun::ClassType> classType,
                                 const std::vector<std::string>& typeParams,
                                 const std::vector<sun::TypePtr>& typeArgs);

 private:
  SemanticContext& ctx_;
  SemanticAnalyzer& analyzer_;
};
