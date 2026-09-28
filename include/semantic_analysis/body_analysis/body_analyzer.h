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
  BodyAnalyzer(SemanticContext &context, SemanticAnalyzer &analyzer)
      : ctx_(context), sema_(analyzer) {}

  /** Check a prepared block in source order. */
  void analyzeBlock(sun::ast::BlockExprAST &block);

  /** Enter a resolved function's scope and check defaults, parameters, and
   * body. */
  void analyzeFunction(sun::ast::FunctionAST &function);

  /** Bind a lambda's parameters and captures before checking its body. */
  void analyzeLambda(sun::ast::LambdaAST &lambda);

  /** Check a specialized class method under its concrete type bindings. */
  void analyzeMethodWithBindings(
      sun::ast::FunctionAST &method,
      std::shared_ptr<sun::types::ClassType> classType,
      const std::vector<std::string> &typeParams,
      const std::vector<sun::types::TypePtr> &typeArgs);

  /**
   * Control flow (analysis_control_flow.cpp)
   */
  void analyzeIfExpr(sun::ast::IfExprAST &ifExpr);

  /**
   * Resolves declarations and checks types in this match expression, recording
   * the results on its syntax nodes.
   */
  void analyzeMatchExpr(sun::ast::MatchExprAST &matchExpr,
                        sun::types::TypePtr expectedType);

  /**
   * Resolves declarations and checks types in this for loop, recording the
   * results on its syntax nodes.
   */
  void analyzeForLoop(sun::ast::ForExprAST &forExpr);

  /**
   * Resolves declarations and checks types in this for in loop, recording the
   * results on its syntax nodes.
   */
  void analyzeForInLoop(sun::ast::ForInExprAST &forInExpr);

  /**
   * Resolves declarations and checks types in this try catch, recording the
   * results on its syntax nodes.
   */
  void analyzeTryCatch(sun::ast::TryCatchExprAST &tryCatchExpr);

  /**
   * Resolves declarations and checks types in this throw expression, recording
   * the results on its syntax nodes.
   */
  void analyzeThrowExpr(sun::ast::ThrowExprAST &throwExpr);

  /**
   * Resolves declarations and checks types in this unsafe block, recording the
   * results on its syntax nodes.
   */
  void analyzeUnsafeBlock(sun::ast::UnsafeBlockAST &unsafeBlock);

  /**
   * Resolves declarations and checks types in this return expression, recording
   * the results on its syntax nodes.
   */
  void analyzeReturnExpr(sun::ast::ReturnExprAST &returnExpr);

  /**
   * Resolves declarations and checks types in this variable assignment,
   * recording the results on its syntax nodes.
   */
  void analyzeVariableAssignment(sun::ast::VariableAssignmentAST &varAssign);

  /**
   * Resolves declarations and checks types in this compound assignment,
   * recording the results on its syntax nodes.
   */
  void analyzeCompoundAssignment(sun::ast::CompoundAssignmentAST &compound);

  /**
   * Resolves declarations and checks types in this member assignment, recording
   * the results on its syntax nodes.
   */
  void analyzeMemberAssignment(sun::ast::MemberAssignmentAST &memberAssign);

  /**
   * Resolves declarations and checks types in this indexed assignment,
   * recording the results on its syntax nodes.
   */
  void analyzeIndexedAssignment(sun::ast::IndexedAssignmentAST &assignment);

  /** Checks statement nodes and returns false for value expressions. */
  bool tryAnalyzeStatement(sun::ast::ExprAST &expr,
                           sun::types::TypePtr expectedType = nullptr);

 private:
  /**
   * Check `mod.name = value`: the target must be a visible, assignable
   * module-level variable, and the value must fit its type. Also records the
   * global's symbol name on the node for codegen.
   */
  void analyzeModuleGlobalAssignment(sun::ast::MemberAssignmentAST &assign,
                                     const sun::types::Type &objectType);

  /**
   * Extract type guard pattern from condition (_is&lt;T&gt;(var)).
   * Returns (varName, narrowedType) if matched.
   */
  std::optional<std::pair<std::string, sun::types::TypePtr>> extractTypeGuard(
      const sun::ast::ExprAST &cond);

  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;
};

}  // namespace sun::semantic_analysis
