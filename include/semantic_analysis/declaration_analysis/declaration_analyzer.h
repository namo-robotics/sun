/** Provides focused checking services for one semantic analysis session. */
#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "semantic_analysis/semantic_context.h"
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticAnalyzer;
/** Checks declarations and signatures and tracks global initializer
 * dependencies. */
class DeclarationAnalyzer {
 public:
  /** Borrows the session context and the cooperating analysis services. */
  DeclarationAnalyzer(SemanticContext &context, SemanticAnalyzer &analyzer)
      : ctx_(context), sema_(analyzer) {}
  /**
   * Extract function signature info (param types, captures, explicit return
   * type). Sets captures on the prototype and handles auto-ref conversion for
   * params. Does NOT register the function — caller is responsible for that.
   * Returns FunctionInfo with returnType set if explicit, nullptr if needs
   * inference. Only bodyless interface requirements may allow an interface
   * return contract; executable functions must return concrete values or views.
   */
  FunctionInfo getFunctionInfo(sun::ast::FunctionAST &func,
                               bool allowInterfaceReturn = false);

  /** The same for a lambda: parameter types, captures, and return type. */
  FunctionInfo getLambdaInfo(sun::ast::LambdaAST &lambda);

  /**
   * Apply FunctionInfo to a prototype (sets captures, param types, return
   * type).
   */
  void applyFunctionInfoToProto(sun::ast::PrototypeAST &proto,
                                const FunctionInfo &info);

  /**
   * Validate that an identifier name is not reserved (doesn't start with '_').
   * Throws an error if the name is reserved.
   */
  void validateNotReserved(const std::string &name, const std::string &kind,
                           std::optional<sun::support::Position> location);

  /**
   * Reject extern signatures that have no C spelling. Primitives,
   * raw_ptr&lt;T&gt;, `ref T` (C's T*) and objects by value all lower
   * correctly; arrays, slices, interfaces and lambdas do not, and must error
   * rather than silently miscompile.
   */
  void validateExternSignature(sun::ast::FunctionAST &func);

  /**
   * Validate that a type parameter exists when the type is a
   * TypeParameterType. Throws an error with source location if the type
   * parameter is not found.
   */
  void validateTypeParameter(const sun::types::TypePtr &type,
                             const sun::ast::ExprAST &node);

  /**
   * Reject a '<'_>' lambda type in return position: its captured
   * environment lives in a stack frame that dies when the function returns.
   */
  void rejectRefEnvReturnType(
      const std::optional<sun::ast::TypeAnnotation> &returnType,
      const sun::support::Position &location, bool allowNamed = false);

  /**
   * Reject any lifetime name the annotation uses (recursively, through
   * element, parameter, return and argument positions) that is neither an
   * active declared name nor the builtin 'this where 'this is legal.
   */
  void checkAnnotationLifetimes(const sun::ast::TypeAnnotation &annot,
                                const sun::support::Position &location);

  /**
   * Validate a signature's lifetime declarations (no duplicates, no
   * collision with the enclosing class's names) and every lifetime name
   * its parameter and return annotations use.
   */
  void checkSignatureLifetimes(const sun::ast::PrototypeAST &proto,
                               const sun::support::Position &location);

  /**
   * Resolves declarations and checks types in this function definition,
   * recording the results on its syntax nodes.
   */
  void analyzeFunctionDefinition(sun::ast::FunctionAST &func);

  /**
   * Resolves declarations and checks types in this module definition, recording
   * the results on its syntax nodes.
   */
  void analyzeModuleDefinition(sun::ast::ModuleAST &nsDecl);

  /**
   * Resolves declarations and checks types in this moon scope, recording the
   * results on its syntax nodes.
   */
  void analyzeMoonScope(sun::ast::ExprAST &expr);

  /**
   * Resolves declarations and checks types in this declare type, recording the
   * results on its syntax nodes.
   */
  void analyzeDeclareType(sun::ast::DeclareTypeAST &declareExpr);

  /**
   * Bindings and assignments (analysis_statements.cpp)
   */
  void analyzeVariableCreation(sun::ast::VariableCreationAST &varCreate);

  /**
   * Analyzes a variable declaration that is not tracked as a pending global:
   * resolves its type, checks its initializer and declares it in the current
   * scope.
   */
  void analyzeVariableDeclaration(sun::ast::VariableCreationAST &varCreate);

  /**
   * Analyzes a file-scope or module-scope variable once, whether the walk
   * over the program reached it or an earlier use asked for it first.
   * `scope` is the file or module scope that declares it. Reports a cycle
   * when the variable's own initializer leads back to it.
   */
  void analyzeGlobal(sun::ast::VariableCreationAST &global,
                     SemanticScope &scope);

  /**
   * Globals may be used before the line that declares them. If `name` refers
   * to a global that has not been analyzed yet, analyzes its declaration now,
   * in the scope it was declared in, so the lookup that follows finds it.
   */
  void ensureGlobalAnalyzed(const std::string &name);

  /** The same, for a global named through its module: `module.name`. */
  void ensureModuleGlobalAnalyzed(const std::string &modulePath,
                                  const std::string &name);

  /**
   * Reports whether the declaration table tracks this node as a global of
   * the current scope. A second declaration of a name is not tracked, and is
   * reported as a duplicate when it is declared.
   */
  bool isTrackedGlobal(const sun::ast::VariableCreationAST &varCreate);

  /**
   * Registers an analyzed module variable under the module's qualified
   * names, so `module.name` finds it.
   */
  void registerModuleVariable(sun::ast::VariableCreationAST &varCreate);

  /**
   * Resolves declarations and checks types in this reference creation,
   * recording the results on its syntax nodes.
   */
  void analyzeReferenceCreation(sun::ast::ReferenceCreationAST &refCreate);

  /** Checks declaration nodes and returns false for other syntax. */
  bool tryAnalyzeDeclaration(sun::ast::ExprAST &expr);

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;
  std::vector<const sun::ast::VariableCreationAST *> globalsInProgress_;

  /**
   * Validate parameter names and resolve their types from prototype.
   * Throws if any parameter name is reserved; applies auto-ref conversion.
   * Returns the resolved param types and sets them on the prototype.
   *
   * allowByValueObjects exempts C externs from REQUIRE_REF_FOR_COMPOUND_PARAMS
   * when that policy is enabled: passing a struct by value is what the C ABI
   * specifies, so it is the callee's signature rather than a Sun choice.
   */
  std::vector<sun::types::TypePtr> validateAndResolveParamTypes(
      sun::ast::PrototypeAST &proto,
      std::optional<sun::support::Position> loc = std::nullopt,
      bool allowByValueObjects = false);

  /**
   * The variables an expression reads but does not bind — what a lambda has
   * to capture. `bound` names the ones already in scope.
   */
  std::set<std::string> collectFreeVariables(
      const sun::ast::ExprAST &expr, const std::set<std::string> &bound);

  /**
   * The same over a block, adding each declaration to `bound` as it is
   * reached so later statements do not count it as free.
   */
  std::set<std::string> collectFreeVariablesInBlock(
      const sun::ast::BlockExprAST &block, std::set<std::string> bound);

  /**
   * The same for a lambda, marking the ones its `[ref x]` list asks to
   * capture by reference.
   */
  std::vector<sun::ast::Capture> buildCaptures(
      const sun::ast::LambdaAST &lambda);
};
}  // namespace sun::semantic_analysis
