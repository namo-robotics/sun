// for_in_expr_ast.h — ForInExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

// for (var x: T in iterable) { ... }
// Iterates over iterable by calling iter() -> IIterator<T>,
// then hasNext() -> bool, next() -> T in a loop
class ForInExprAST : public ExprAST {
  std::string LoopVar;                // Variable name (x)
  TypeAnnotation LoopVarType;         // Type annotation (T)
  std::unique_ptr<ExprAST> Iterable;  // Expression that yields an iterable
  std::unique_ptr<ExprAST> Body;

 protected:
  // Override to allocate ForInAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<ForInAnalysis>();
    }
  }

 private:
  // Access as ForInAnalysis
  ForInAnalysis& forInAnalysis() const {
    ensureAnalysis();
    return static_cast<ForInAnalysis&>(*analysis_);
  }

 public:
  ForInExprAST(std::string LoopVar, TypeAnnotation LoopVarType,
               std::unique_ptr<ExprAST> Iterable, std::unique_ptr<ExprAST> Body)
      : LoopVar(std::move(LoopVar)),
        LoopVarType(std::move(LoopVarType)),
        Iterable(std::move(Iterable)),
        Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::FOR_IN_LOOP; }
  std::string toString() const override {
    return "for (var " + LoopVar + ": " + LoopVarType.toString() + " in " +
           Iterable->toString() + ") " + Body->toString();
  }

  const std::string& getLoopVar() const { return LoopVar; }
  const TypeAnnotation& getLoopVarType() const { return LoopVarType; }
  const ExprAST* getIterable() const { return Iterable.get(); }
  const ExprAST* getBody() const { return Body.get(); }

  // Resolved loop variable type (set by semantic analyzer)
  void setResolvedLoopVarType(sun::TypePtr type) const {
    forInAnalysis().resolvedLoopVarType = std::move(type);
  }
  sun::TypePtr getResolvedLoopVarType() const {
    return analysis_
               ? static_cast<ForInAnalysis&>(*analysis_).resolvedLoopVarType
               : nullptr;
  }
  bool hasResolvedLoopVarType() const {
    return analysis_ &&
           static_cast<ForInAnalysis&>(*analysis_).resolvedLoopVarType !=
               nullptr;
  }

  std::string dotLabel() const override { return "ForIn\n" + LoopVar; }
};
