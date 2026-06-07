// module_ast.h — ModuleAST class

#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"

// Module declaration: module Name { declarations... }
// Also supports legacy 'namespace' keyword
class ModuleAST : public ExprAST {
  std::string name;
  std::unique_ptr<BlockExprAST> body;

 public:
  ModuleAST(std::string name, std::unique_ptr<BlockExprAST> body)
      : name(std::move(name)), body(std::move(body)) {}

  ASTNodeType getType() const override { return ASTNodeType::MODULE; }
  std::string toString() const override {
    return "module " + name + " " + body->toString();
  }

  const std::string& getName() const { return name; }
  const BlockExprAST& getBody() const { return *body; }
  std::string dotLabel() const override { return "Module\n" + name; }
  std::unique_ptr<ExprAST> clone() const override;
};
