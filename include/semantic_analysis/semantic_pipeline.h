#pragma once

#include "semantic_analysis/body_analysis_pass.h"
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

  /** Name generated declarations with the same pass used for source code. */
  const DeclarationNamingPass& naming() const { return declarationNamingPass_; }

  /** Access the pass that registers declarations before body checking. */
  DeclarationCollectionPass& declarations() {
    return declarationCollectionPass_;
  }

  /** Access the pass that checks prepared bodies. */
  BodyAnalysisPass& bodies() { return bodyAnalysisPass_; }

 private:
  SemanticContext& context_;
  FieldInitializerPreparationPass fieldInitializerPreparationPass_;
  DeclarationNamingPass declarationNamingPass_;
  DeclarationCollectionPass declarationCollectionPass_;
  LocalDeclarationNamingPass localDeclarationNamingPass_;
  BodyAnalysisPass bodyAnalysisPass_;
};

}  // namespace sun
