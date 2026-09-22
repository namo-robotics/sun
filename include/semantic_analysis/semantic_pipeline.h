#pragma once

#include <functional>

#include "semantic_analysis/passes/body_analysis_pass.h"
#include "semantic_analysis/passes/declaration_collection_pass.h"
#include "semantic_analysis/passes/declaration_identity_pass.h"
#include "semantic_analysis/passes/declaration_naming_pass.h"
#include "semantic_analysis/passes/field_initializer_preparation_pass.h"
#include "semantic_analysis/passes/global_initializer_evaluation_pass.h"
#include "semantic_analysis/passes/global_registration_pass.h"
#include "semantic_analysis/passes/import_completion_pass.h"
#include "semantic_analysis/passes/import_dependency_validation_pass.h"
#include "semantic_analysis/passes/import_record_registration_pass.h"
#include "semantic_analysis/passes/type_registration_pass.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::ast::ExprAST;

class SemanticAnalyzer;

}  // namespace sun::semantic_analysis

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Run import preparation, then source analysis, in one shared session. */
class SemanticPipeline {
 public:
  /** Borrow the owning analyzer's shared context and checking helpers. */
  explicit SemanticPipeline(sun::semantic_analysis::SemanticAnalyzer& analyzer);

  /** Keep pass references tied to the context and helpers they were given. */
  SemanticPipeline(const SemanticPipeline&) = delete;
  /** Disallows assignment so ownership and object identity cannot be
   * duplicated. */
  SemanticPipeline& operator=(const SemanticPipeline&) = delete;

  /** Prepare imports and names, collect declarations, and check bodies in a
   * complete AST whose bundles have already been loaded. Performs no file I/O.
   */
  void run(sun::ast::BlockExprAST& block,
           const std::function<void()>& declarationsReady = {});

  /** Assign identities to newly generated syntax before resolving it. */
  void prepareGenerated(const ExprAST& expression, DeclarationId owner = {},
                        const ExprAST* origin = nullptr);

  /** Prepare generated method identities and names in their enclosing scope. */
  void prepareGenerated(ExprAST& expression,
                        const std::vector<std::string>& scope,
                        DeclarationId owner = {},
                        const ExprAST* origin = nullptr);

  /** Access the pass that registers declarations before body checking. */
  passes::DeclarationCollectionPass& declarations() {
    return declarationCollectionPass_;
  }

 private:
  /** Returns borrowed pointers to the top-level imported bundles, excluding
   * the bundle being built. Does not register declarations or modify the AST.
   */
  static std::vector<sun::ast::MoonScopeAST*> getMoonImports(
      const sun::ast::BlockExprAST& block);

  /** Prepare every imported bundle's identities, types, signatures, and
   * declaration bindings before source analysis. Uses the supplied AST only;
   * source declarations and the bundle being built are left for later stages.
   */
  void prepareImports(sun::ast::BlockExprAST& block);

  sun::semantic_analysis::SemanticAnalyzer& analyzer_;
  sun::semantic_analysis::SemanticContext& context_;
  passes::ImportRecordRegistrationPass importRecordRegistrationPass_;
  passes::ImportDependencyValidationPass importDependencyValidationPass_;
  passes::ImportCompletionPass importCompletionPass_;
  passes::FieldInitializerPreparationPass fieldInitializerPreparationPass_;
  passes::DeclarationIdentityPass declarationIdentityPass_;
  passes::DeclarationNamingPass declarationNamingPass_;
  passes::GlobalRegistrationPass globalRegistrationPass_;
  passes::TypeRegistrationPass typeRegistrationPass_;
  passes::DeclarationCollectionPass declarationCollectionPass_;
  passes::BodyAnalysisPass bodyAnalysisPass_;
  passes::GlobalInitializerEvaluationPass globalInitializerEvaluationPass_;
};

}  // namespace sun::semantic_analysis
