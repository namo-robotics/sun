// call_expr_ast.h — CallExprAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"
#include "types.h"

// Forward declaration for dotLabel()
class MemberAccessAST;
class VariableReferenceAST;

// Unified call expression - callee is any expression that evaluates to a
// function This handles both direct calls (foo(x)) and indirect calls
// (myFuncVar(x))
class CallExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Callee;  // Expression that evaluates to a function
  std::vector<std::unique_ptr<ExprAST>> Args;

 public:
  CallExprAST(std::unique_ptr<ExprAST> Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(std::move(Callee)), Args(std::move(Args)) {}
  ASTNodeType getType() const override { return ASTNodeType::CALL; }
  std::string toString() const override {
    std::string result = Callee->toString() + "(";
    for (size_t i = 0; i < Args.size(); ++i) {
      if (i > 0) result += ", ";
      result += Args[i]->toString();
    }
    return result + ")";
  }
  const ExprAST* getCallee() const { return Callee.get(); }
  const std::vector<std::unique_ptr<ExprAST>>& getArgs() const { return Args; }

  // Mutable access to the argument list (used to expand variadic packs into
  // concrete args during semantic analysis).
  std::vector<std::unique_ptr<ExprAST>>& getArgsMutable() { return Args; }

  // Returns the resolved types of all arguments (for constructor overload resolution)
  std::vector<sun::TypePtr> getResolvedArgTypes() const {
    std::vector<sun::TypePtr> types;
    types.reserve(Args.size());
    for (const auto& arg : Args) {
      types.push_back(arg->getResolvedType());
    }
    return types;
  }

  std::string dotLabel() const override;
};
