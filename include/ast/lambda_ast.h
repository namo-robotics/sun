// lambda_ast.h — LambdaAST class

#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "ast/prototype_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Lambda expression (anonymous function)
 */
class LambdaAST : public ExprAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<BlockExprAST> Body;

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  LambdaAST(std::unique_ptr<PrototypeAST> Proto,
            std::unique_ptr<BlockExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::LAMBDA; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (Body) Body->forEachChildSlot(fn);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result;
    const auto& lifetimes = Proto->getLifetimeParameters();
    if (!lifetimes.empty()) {
      result += "<";
      for (size_t i = 0; i < lifetimes.size(); ++i) {
        if (i > 0) result += ", ";
        result += lifetimes[i].toString();
      }
      result += ">";
    }

    const auto& refs = Proto->getRefCaptureNames();
    const auto& owned = Proto->getOwnedCaptureNames();
    if (!refs.empty() || !owned.empty()) {
      if (!lifetimes.empty()) result += " ";
      result += "[";
      for (size_t i = 0; i < refs.size(); ++i) {
        if (i > 0) result += ", ";
        if (Proto->isConstRefCapture(refs[i])) result += "const ";
        result += "ref " + refs[i];
      }
      for (size_t i = 0; i < owned.size(); ++i) {
        if (i > 0 || !refs.empty()) result += ", ";
        result += owned[i];
      }
      result += "]";
    }

    result += "(";
    const auto& args = Proto->getArgs();
    for (size_t i = 0; i < args.size(); ++i) {
      if (i > 0) result += ", ";
      result += args[i].first + ": " + args[i].second.toString();
    }
    result += ") =>";
    if (Proto->hasReturnType())
      result += " " + Proto->getReturnType()->toString();
    if (Body) result += " " + Body->toString();
    return result;
  }

  /** A function and its prototype denote the same declaration. */
  sun::semantic_analysis::DeclarationId getDeclarationId() const override {
    return Proto->getDeclarationId();
  }
  /** Assign the prototype's declaration identity. */
  void setDeclarationId(
      sun::semantic_analysis::DeclarationId id) const override {
    Proto->setDeclarationId(id);
  }
  /** Access the identities owned by the prototype. */
  sun::semantic_analysis::DeclarationIdentity& declarationIdentity()
      const override {
    return Proto->declarationIdentity();
  }
  /** Clear body and signature results while retaining identities. */
  void clearComputedAnalysis() const override {
    ExprAST::clearComputedAnalysis();
    Proto->clearComputedAnalysis();
  }
  /** Discard the function's annotations and its prototype's annotations. */
  void resetAnalysisSession() const override {
    ExprAST::resetAnalysisSession();
    Proto->resetAnalysisSession();
  }
  /** Provides the function signature independently of its body. */
  const PrototypeAST& getProto() const { return *Proto; }
  /** Provides access to the expressions that make up the body. */
  const BlockExprAST& getBody() const { return *Body; }
  /** Reports whether a function body is present rather than just a declaration. */
  bool hasBody() const { return Body != nullptr; }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Lambda"; }
};

}  // namespace sun::ast
