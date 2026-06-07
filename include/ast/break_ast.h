// break_ast.h — BreakAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Break statement: break;
// Exits the innermost enclosing loop
class BreakAST : public ExprAST {
 public:
  BreakAST() = default;

  ASTNodeType getType() const override { return ASTNodeType::BREAK_STMT; }
  std::string toString() const override { return "break"; }
  std::string dotLabel() const override { return "Break"; }
  std::unique_ptr<ExprAST> clone() const override;
};
