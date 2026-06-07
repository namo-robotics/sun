// continue_ast.h — ContinueAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Continue statement: continue;
// Jumps to the next iteration of the innermost enclosing loop
class ContinueAST : public ExprAST {
 public:
  ContinueAST() = default;

  ASTNodeType getType() const override { return ASTNodeType::CONTINUE_STMT; }
  std::string toString() const override { return "continue"; }
  std::string dotLabel() const override { return "Continue"; }
  std::unique_ptr<ExprAST> clone() const override;
};
