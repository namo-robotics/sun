/** Implements storage and control-flow queries on analyzed expressions. */
#include "semantic_analysis/expression_properties.h"

/** Checks expression properties during semantic analysis. */
namespace sun::semantic_analysis {
using sun::types::TypePtr;

using sun::ast::ASTNodeType;

/** Reports whether an expression denotes storage that can be borrowed. */
bool isBorrowableLvalue(const ExprAST& target) {
  ASTNodeType kind = target.getType();
  // A conditional picks one of two slots at runtime; it borrows if both
  // branches do.
  if (kind == ASTNodeType::TERNARY) {
    const auto& ternary = static_cast<const sun::ast::TernaryExprAST&>(target);
    return isBorrowableLvalue(*ternary.getThen()) &&
           isBorrowableLvalue(*ternary.getElse());
  }
  return kind == ASTNodeType::VARIABLE_REFERENCE ||
         kind == ASTNodeType::MEMBER_ACCESS || kind == ASTNodeType::INDEX;
}

/** Reports whether an expression always leaves the current control-flow path.
 */
bool alwaysExits(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::RETURN:
    case ASTNodeType::THROW:
      return true;
    case ASTNodeType::BLOCK: {
      // One exiting statement is enough: nothing after it runs
      const auto& block = static_cast<const sun::ast::BlockExprAST&>(expr);
      for (const auto& stmt : block.getBody()) {
        if (alwaysExits(*stmt)) return true;
      }
      return false;
    }
    case ASTNodeType::UNSAFE_BLOCK:
      return alwaysExits(
          static_cast<const sun::ast::UnsafeBlockAST&>(expr).getBody());
    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const sun::ast::IfExprAST&>(expr);
      return ifExpr.getElse() != nullptr && alwaysExits(*ifExpr.getThen()) &&
             alwaysExits(*ifExpr.getElse());
    }
    case ASTNodeType::MATCH: {
      // Every arm must exit, and no discriminant value may slip past the
      // arms: an enum match is checked for exhaustiveness elsewhere, and any
      // other match needs a wildcard to promise the same.
      const auto& matchExpr = static_cast<const sun::ast::MatchExprAST&>(expr);
      if (matchExpr.getArms().empty()) return false;
      bool sawWildcard = false;
      for (const auto& arm : matchExpr.getArms()) {
        if (!alwaysExits(*arm.body)) return false;
        if (arm.isWildcard) sawWildcard = true;
      }
      TypePtr discType =
          sun::types::unwrapRef(matchExpr.getDiscriminant()->getResolvedType());
      return sawWildcard || (discType && discType->isEnum());
    }
    case ASTNodeType::TRY_CATCH: {
      // The body may stop part-way and land in a catch, so every clause has
      // to exit as well
      const auto& tryCatch =
          static_cast<const sun::ast::TryCatchExprAST&>(expr);
      if (!alwaysExits(tryCatch.getTryBlock())) return false;
      for (const auto& clause : tryCatch.getCatchClauses()) {
        if (!clause.body || !alwaysExits(*clause.body)) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

}  // namespace sun::semantic_analysis
