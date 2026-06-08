// spawn_expr_ast.h — SpawnExprAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Spawn expression: spawn(lambda) - creates an OS thread
// Takes a lambda expression and returns Thread<T> where T is the lambda's
// return type The thread executes the lambda concurrently; use thread.join() to
// wait and get result
class SpawnExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Lambda;  // The lambda to execute in the new thread

 public:
  explicit SpawnExprAST(std::unique_ptr<ExprAST> lambda)
      : Lambda(std::move(lambda)) {}

  ASTNodeType getType() const override { return ASTNodeType::SPAWN; }
  std::string toString() const override {
    return "spawn(" + Lambda->toString() + ")";
  }

  const ExprAST& getLambda() const { return *Lambda; }
  std::string dotLabel() const override { return "Spawn"; }
};
