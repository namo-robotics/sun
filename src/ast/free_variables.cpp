// free_variables.cpp — Names an expression uses but does not declare

#include "ast/free_variables.h"

#include "ast.h"
#include "ast/ast_children.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

// Names the expression uses that nothing inside it declares.
//
// Most nodes are just a shape to walk through, so `forEachChild` — the
// enumerator that already knows every node's children — supplies the default.
// Only the handful of nodes that mention a name, or that declare one their
// children can see, need their own case here. Anything left out of both is
// silently treated as capturing nothing, which is how a lambda ends up
// looking for a local among the module's globals.
std::set<std::string> collectFreeVariables(const ExprAST& expr,
                                           const std::set<std::string>& bound) {
  std::set<std::string> free;

  auto collectFrom = [&](const ExprAST& child,
                         const std::set<std::string>& childBound) {
    auto childFree = collectFreeVariables(child, childBound);
    free.insert(childFree.begin(), childFree.end());
  };

  switch (expr.getType()) {
    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& varRef = static_cast<const VariableReferenceAST&>(expr);
      if (!bound.count(varRef.getName())) {
        free.insert(varRef.getName());
      }
      break;
    }

    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      const auto& varAssign = static_cast<const VariableAssignmentAST&>(expr);
      // Writing to a name uses it just as reading does
      if (!bound.count(varAssign.getName())) {
        free.insert(varAssign.getName());
      }
      collectFrom(*varAssign.getValue(), bound);
      break;
    }

    case ASTNodeType::BLOCK: {
      const auto& block = static_cast<const BlockExprAST&>(expr);
      auto blockFree = collectFreeVariablesInBlock(block, bound);
      free.insert(blockFree.begin(), blockFree.end());
      break;
    }

    case ASTNodeType::FOR_LOOP: {
      const auto& forExpr = static_cast<const ForExprAST&>(expr);
      std::set<std::string> innerBound = bound;
      if (forExpr.getInit()) {
        collectFrom(*forExpr.getInit(), bound);
        // A loop counter declared in the header is visible to the rest of it
        if (forExpr.getInit()->getType() == ASTNodeType::VARIABLE_CREATION) {
          innerBound.insert(
              static_cast<const VariableCreationAST&>(*forExpr.getInit())
                  .getName());
        }
      }
      if (forExpr.getCondition())
        collectFrom(*forExpr.getCondition(), innerBound);
      if (forExpr.getIncrement())
        collectFrom(*forExpr.getIncrement(), innerBound);
      collectFrom(*forExpr.getBody(), innerBound);
      break;
    }

    case ASTNodeType::FOR_IN_LOOP: {
      const auto& forInExpr = static_cast<const ForInExprAST&>(expr);
      collectFrom(*forInExpr.getIterable(), bound);
      // The loop variable is declared by the loop, not captured from outside
      std::set<std::string> bodyBound = bound;
      bodyBound.insert(forInExpr.getLoopVar());
      collectFrom(*forInExpr.getBody(), bodyBound);
      break;
    }

    case ASTNodeType::TRY_CATCH: {
      const auto& tryCatch = static_cast<const TryCatchExprAST&>(expr);
      collectFrom(tryCatch.getTryBlock(), bound);
      for (const auto& clause : tryCatch.getCatchClauses()) {
        // The caught error is declared by the clause
        std::set<std::string> clauseBound = bound;
        clauseBound.insert(clause.bindingName);
        collectFrom(*clause.body, clauseBound);
      }
      break;
    }

    case ASTNodeType::LAMBDA: {
      // A nested lambda's free variables (minus its own params) are free in
      // the enclosing scope too: the enclosing closure must capture them so
      // the inner closure can initialize its env from the enclosing one
      const auto& lambda = static_cast<const LambdaAST&>(expr);
      std::set<std::string> innerBound = bound;
      for (const auto& arg : lambda.getProto().getArgNames()) {
        innerBound.insert(arg);
      }
      auto innerFree =
          collectFreeVariablesInBlock(lambda.getBody(), innerBound);
      free.insert(innerFree.begin(), innerFree.end());
      break;
    }

    // Definitions carry their own scope and cannot reach an enclosing local
    case ASTNodeType::FUNCTION:
    case ASTNodeType::CLASS_DEFINITION:
    case ASTNodeType::INTERFACE_DEFINITION:
    case ASTNodeType::ENUM_DEFINITION:
    case ASTNodeType::MODULE:
    case ASTNodeType::MOON_SCOPE:
    case ASTNodeType::IMPORT_SCOPE:
      break;

    default:
      forEachChild(expr,
                   [&](const ExprAST& child) { collectFrom(child, bound); });
      break;
  }

  return free;
}

/** Walks a block in order, binding each declaration for the statements after
 * it. */
std::set<std::string> collectFreeVariablesInBlock(const BlockExprAST& block,
                                                  std::set<std::string> bound) {
  std::set<std::string> free;

  for (const auto& expr : block.getBody()) {
    // Skip nested functions for now - they handle their own captures
    if (expr->isFunction()) {
      continue;
    }

    auto exprFree = collectFreeVariables(*expr, bound);
    free.insert(exprFree.begin(), exprFree.end());

    // Variable creation adds to bound set for subsequent expressions
    if (expr->getType() == ASTNodeType::VARIABLE_CREATION) {
      const auto& varCreate = static_cast<const VariableCreationAST&>(*expr);
      bound.insert(varCreate.getName());
    }
  }

  return free;
}

}  // namespace sun::ast
