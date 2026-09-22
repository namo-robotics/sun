#pragma once

#include <functional>

#include "semantic_analysis/passes/declaration_collection_pass.h"
#include "semantic_analysis/passes/declaration_naming_pass.h"
#include "semantic_analysis/passes/field_initializer_preparation_pass.h"
#include "semantic_analysis/passes/moon_import_preparation_pass.h"
#include "semantic_analysis/passes/type_registration_pass.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::ast::ExprAST;

class SemanticAnalyzer;

}  // namespace sun::semantic_analysis

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Own and order the passes for one semantic analysis session. */
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
  /**
   * Records every file-scope and module-scope variable the source declares
   * in the declaration table, under its qualified name. Globals can be used
   * before the line that declares them, and this is how such a use finds the
   * declaration. Runs once names have been assigned; nothing is analyzed.
   */
  void registerGlobals(const sun::ast::BlockExprAST& block);

  sun::semantic_analysis::SemanticAnalyzer& analyzer_;
  sun::semantic_analysis::SemanticContext& context_;
  passes::MoonImportPreparationPass moonImportPreparationPass_;
  passes::FieldInitializerPreparationPass fieldInitializerPreparationPass_;
  passes::DeclarationNamingPass declarationNamingPass_;
  passes::TypeRegistrationPass typeRegistrationPass_;
  passes::DeclarationCollectionPass declarationCollectionPass_;
};

}  // namespace sun::semantic_analysis
