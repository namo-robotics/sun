#pragma once

#include <functional>

#include "semantic_analysis/passes/declaration_collection_pass.h"
#include "semantic_analysis/passes/declaration_naming_pass.h"
#include "semantic_analysis/passes/field_initializer_preparation_pass.h"

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
  /** Disallows assignment so ownership and object identity cannot be duplicated. */
  SemanticPipeline& operator=(const SemanticPipeline&) = delete;

  /** Prepare names, collect declarations, and check the program's bodies. */
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
  sun::semantic_analysis::SemanticAnalyzer& analyzer_;
  sun::semantic_analysis::SemanticContext& context_;
  passes::FieldInitializerPreparationPass fieldInitializerPreparationPass_;
  passes::DeclarationNamingPass declarationNamingPass_;
  passes::DeclarationCollectionPass declarationCollectionPass_;
};

}  // namespace sun::semantic_analysis
