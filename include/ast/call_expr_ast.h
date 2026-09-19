// call_expr_ast.h — CallExprAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"
#include "types/types.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

// Forward declaration for dotLabel()
class MemberAccessAST;
class VariableReferenceAST;

/**
 * Unified call expression - callee is any expression that evaluates to a
 * function This handles both direct calls (foo(x)) and indirect calls
 * (myFuncVar(x))
 */
class CallExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Callee;  // Expression that evaluates to a function
  std::vector<std::unique_ptr<ExprAST>> Args;

 protected:
  /** Creates the analysis annotations required by this node when first needed. */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<CallAnalysis>();
    }
  }

 private:
  /** Accesses the call resolution and argument conversion annotations. */
  CallAnalysis& callAnalysis() const {
    ensureAnalysis();
    return static_cast<CallAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  CallExprAST(std::unique_ptr<ExprAST> Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(std::move(Callee)), Args(std::move(Args)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::CALL; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = Callee->toString() + "(";
    for (size_t i = 0; i < Args.size(); ++i) {
      if (i > 0) result += ", ";
      result += Args[i]->toString();
    }
    return result + ")";
  }
  /** Returns the callee stored by this object. */
  const ExprAST* getCallee() const { return Callee.get(); }
  /** Provides the ordered arguments associated with this expression. */
  const std::vector<std::unique_ptr<ExprAST>>& getArgs() const { return Args; }

  /**
   * Mutable access to the argument list (used to expand variadic packs into
   * concrete args during semantic analysis).
   */
  std::vector<std::unique_ptr<ExprAST>>& getArgsMutable() { return Args; }

  /**
   * How each argument reaches its parameter (set by the semantic analyzer
   * once the callee's signature is known; one entry per argument)
   */
  void setArgConversions(
      std::vector<sun::semantic_analysis::ArgConversion> conversions) const {
    callAnalysis().argConversions = std::move(conversions);
  }
  /** Returns the argument conversions selected by analysis. */
  const std::vector<sun::semantic_analysis::ArgConversion>& getArgConversions()
      const {
    return callAnalysis().argConversions;
  }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(Callee);
    for (auto& arg : Args) fn(arg);
  }

  /**
   * Returns the resolved types of all arguments (for constructor overload
   * resolution)
   */
  std::vector<sun::types::TypePtr> getResolvedArgTypes() const {
    std::vector<sun::types::TypePtr> types;
    types.reserve(Args.size());
    for (const auto& arg : Args) {
      types.push_back(arg->getResolvedType());
    }
    return types;
  }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override;
};

}  // namespace sun::ast
