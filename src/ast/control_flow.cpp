// control_flow.cpp — Questions about how control moves through an expression

#include "ast/control_flow.h"

#include "ast.h"

bool exprDiverges(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::RETURN:
    case ASTNodeType::THROW:
      return true;
    case ASTNodeType::BLOCK: {
      const auto& block = static_cast<const BlockExprAST&>(expr);
      for (const auto& stmt : block.getBody()) {
        if (stmt && exprDiverges(*stmt)) return true;
      }
      return false;
    }
    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const IfExprAST&>(expr);
      return ifExpr.getThen() && ifExpr.getElse() &&
             exprDiverges(*ifExpr.getThen()) && exprDiverges(*ifExpr.getElse());
    }
    default:
      return false;
  }
}
