#pragma once

#include <functional>

#include "semantic_analysis/declaration_collection_pass.h"
#include "semantic_analysis/declaration_naming_pass.h"
#include "semantic_analysis/field_initializer_preparation_pass.h"

namespace sun::semantic_analysis {
using sun::ast::ExprAST;

class SemanticAnalyzer;

}  // namespace sun::semantic_analysis

namespace sun::semantic_analysis {

/** Own and order the passes for one semantic analysis session. */
class SemanticPipeline {
 public:
  /** Borrow the owning analyzer's shared context and checking helpers. */
  explicit SemanticPipeline(sun::semantic_analysis::SemanticAnalyzer& analyzer);

  /** Keep pass references tied to the context and helpers they were given. */
  SemanticPipeline(const SemanticPipeline&) = delete;
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
  sun::semantic_analysis::DeclarationCollectionPass& declarations() {
    return declarationCollectionPass_;
  }

 private:
  sun::semantic_analysis::SemanticAnalyzer& analyzer_;
  sun::semantic_analysis::SemanticContext& context_;
  FieldInitializerPreparationPass fieldInitializerPreparationPass_;
  DeclarationNamingPass declarationNamingPass_;
  sun::semantic_analysis::DeclarationCollectionPass declarationCollectionPass_;
};

}  // namespace sun::semantic_analysis
