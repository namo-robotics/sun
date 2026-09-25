// control_flow.cpp — Questions about how control moves through an expression

#include "ast/control_flow.h"

#include "ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** Reports whether evaluating this expression always exits the current
 * control-flow path. A break or continue counts: code after it in the same
 * block or arm never runs, even though the enclosing loop carries on. */
bool exprDiverges(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::RETURN:
    case ASTNodeType::THROW:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
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

}  // namespace sun::ast
