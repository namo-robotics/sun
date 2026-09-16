#pragma once

#include "semantic_analysis/local_declaration_naming_pass.h"
#include "semantic_analysis/semantic_context.h"

class SemanticAnalyzer;

/** Check prepared statements and manage function-body scopes and local passes.
 */
class BodyAnalysisPass {
 public:
  /** Borrow the shared context, checking helpers, and local naming pass. */
  BodyAnalysisPass(SemanticContext& context, SemanticAnalyzer& analyzer,
                   const sun::LocalDeclarationNamingPass& localNaming)
      : ctx_(context),
        analyzer_(analyzer),
        localDeclarationNamingPass_(localNaming) {}

  /** Check a prepared block in source order. */
  void run(BlockExprAST& block);

  /** Name locals and check a specialization body in its established function
   * scope.
   */
  void runInFunctionScope(BlockExprAST& body);

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
  const sun::LocalDeclarationNamingPass& localDeclarationNamingPass_;
};
