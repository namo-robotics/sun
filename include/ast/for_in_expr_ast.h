// for_in_expr_ast.h — ForInExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * for (var x: T in iterable) { ... }
 * Iterates over iterable by calling iter() -> IIterator<T, C> (when the
 * iterable is not itself an iterator), then next(ref C) -> Option<T> until
 * None
 */
class ForInExprAST : public ExprAST {
  std::string LoopVar;                // Variable name (x)
  TypeAnnotation LoopVarType;         // Type annotation (T)
  std::unique_ptr<ExprAST> Iterable;  // Expression that yields an iterable
  std::unique_ptr<ExprAST> Body;
  bool isConst_;  // `for (const x: T in ...)`

 protected:
  /**
   * Override to allocate ForInAnalysis instead of base ExprAnalysis
   */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<ForInAnalysis>();
    }
  }

 public:
  /** Access the resolved iterator protocol and loop binding. */
  ForInAnalysis& forInAnalysis() const {
    ensureAnalysis();
    return static_cast<ForInAnalysis&>(*analysis_);
  }

  /** Creates this syntax node from its operands and declaration information. */
  ForInExprAST(std::string LoopVar, TypeAnnotation LoopVarType,
               std::unique_ptr<ExprAST> Iterable, std::unique_ptr<ExprAST> Body,
               bool isConst = false)
      : LoopVar(std::move(LoopVar)),
        LoopVarType(std::move(LoopVarType)),
        Iterable(std::move(Iterable)),
        Body(std::move(Body)),
        isConst_(isConst) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::FOR_IN_LOOP; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(Iterable);
    fn(Body);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return std::string(isConst_ ? "for (const " : "for (var ") + LoopVar +
           ": " + LoopVarType.toString() + " in " + Iterable->toString() +
           ") " + Body->toString();
  }

  /** Reports whether this syntax node represents const. */
  bool isConst() const { return isConst_; }
  /** Returns the loop var stored by this object. */
  const std::string& getLoopVar() const { return LoopVar; }
  /** Returns the loop var type stored by this object. */
  const TypeAnnotation& getLoopVarType() const { return LoopVarType; }
  /** Returns the iterable stored by this object. */
  const ExprAST* getIterable() const { return Iterable.get(); }
  /** Provides access to the expressions that make up the body. */
  const ExprAST* getBody() const { return Body.get(); }

  /**
   * Mutable body slot for LoweringPass block normalization
   */
  std::unique_ptr<ExprAST>& bodySlot() { return Body; }

  /**
   * Resolved loop variable type (set by semantic analyzer)
   */
  void setResolvedLoopVarType(sun::semantic_analysis::TypePtr type) const {
    forInAnalysis().resolvedLoopVarType = std::move(type);
  }
  /** Returns the resolved loop var type stored by this object. */
  sun::semantic_analysis::TypePtr getResolvedLoopVarType() const {
    return analysis_
               ? static_cast<ForInAnalysis&>(*analysis_).resolvedLoopVarType
               : nullptr;
  }
  /** Reports whether this object has resolved loop var type. */
  bool hasResolvedLoopVarType() const {
    return analysis_ &&
           static_cast<ForInAnalysis&>(*analysis_).resolvedLoopVarType !=
               nullptr;
  }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "ForIn\n" + LoopVar; }
};

}  // namespace sun::ast
