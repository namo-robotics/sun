#pragma once

#include "semantic_analysis/declaration_collection_pass.h"
#include "semantic_analysis/declaration_naming_pass.h"
#include "semantic_analysis/field_initializer_preparation_pass.h"

class SemanticAnalyzer;

namespace sun {

/** Own and order the passes for one semantic analysis session. */
class SemanticPipeline {
 public:
  /** Borrow the owning analyzer's shared context and checking helpers. */
  explicit SemanticPipeline(SemanticAnalyzer& analyzer);

  /** Keep pass references tied to the context and helpers they were given. */
  SemanticPipeline(const SemanticPipeline&) = delete;
  SemanticPipeline& operator=(const SemanticPipeline&) = delete;

  /** Prepare names, collect declarations, and check the program's bodies. */
  void run(BlockExprAST& block);

  /** Assign identities to newly generated syntax before resolving it. */
  void prepareGenerated(const ExprAST& expression, DeclarationId owner = {},
                        const ExprAST* origin = nullptr);

  /** Prepare generated method identities and names in their enclosing scope. */
  void prepareGenerated(ExprAST& expression,
                        const std::vector<std::string>& scope,
                        const std::vector<std::string>& module,
                        DeclarationId owner = {},
                        const ExprAST* origin = nullptr);

  /** Access the pass that registers declarations before body checking. */
  DeclarationCollectionPass& declarations() {
    return declarationCollectionPass_;
  }

 private:
  SemanticAnalyzer& analyzer_;
  SemanticContext& context_;
  FieldInitializerPreparationPass fieldInitializerPreparationPass_;
  DeclarationNamingPass declarationNamingPass_;
  DeclarationCollectionPass declarationCollectionPass_;
};

}  // namespace sun
