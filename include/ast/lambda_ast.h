// lambda_ast.h — LambdaAST class

#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "ast/prototype_ast.h"

// Lambda expression (anonymous function)
class LambdaAST : public ExprAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<BlockExprAST> Body;

 public:
  LambdaAST(std::unique_ptr<PrototypeAST> Proto,
            std::unique_ptr<BlockExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::LAMBDA; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (Body) Body->forEachChildSlot(fn);
  }
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

  const PrototypeAST& getProto() const { return *Proto; }
  const BlockExprAST& getBody() const { return *Body; }
  bool hasBody() const { return Body != nullptr; }

  std::string dotLabel() const override { return "Lambda"; }
};
