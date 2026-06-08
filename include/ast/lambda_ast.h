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
  std::string toString() const override {
    std::string result = "lambda(";
    const auto& args = Proto->getArgs();
    for (size_t i = 0; i < args.size(); ++i) {
      if (i > 0) result += ", ";
      result += args[i].first + ": " + args[i].second.toString();
    }
    result += ")";
    if (Proto->hasReturnType())
      result += " " + Proto->getReturnType()->toString();
    if (Body) result += " " + Body->toString();
    return result;
  }

  const PrototypeAST& getProto() const { return *Proto; }
  const BlockExprAST& getBody() const { return *Body; }
  bool hasBody() const { return Body != nullptr; }

  std::string dotLabel() const override { return "Lambda"; }
  std::unique_ptr<ExprAST> clone() const override;
};
