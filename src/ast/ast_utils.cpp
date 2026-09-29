// ast_utils.cpp — Common utility functions for AST operations

#include "ast/ast_utils.h"

#include "ast.h"
#include "ast/ast_children.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** Clears this node's type, its match arm annotations, then every child. */
void clearResolvedTypes(const ExprAST& expr) {
  expr.clearResolvedType();
  if (expr.getType() == ASTNodeType::MATCH) {
    // Arm tags and binding types live on the arms, not on child nodes
    auto& match =
        const_cast<MatchExprAST&>(static_cast<const MatchExprAST&>(expr));
    for (auto& arm : match.getArmsMutable()) {
      arm.resolvedVariantTag = -1;
      for (auto& binding : arm.bindings) binding.resolvedType = nullptr;
    }
  }
  forEachChild(expr, [](const ExprAST& child) { clearResolvedTypes(child); });
}

}  // namespace sun::ast
