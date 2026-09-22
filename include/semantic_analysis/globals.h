// globals.h — Finding the file-scope variables of an analyzed program.

#pragma once

#include "ast.h"
#include "semantic_analysis/declaration_table.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/**
 * Calls fn with each variable a block declares at file scope, in source
 * order, looking inside modules and bundle scopes. This is also the order in
 * which startup initializes them.
 */
template <typename Fn>
void forEachGlobalDeclaration(const sun::ast::BlockExprAST& block, Fn&& fn) {
  for (const auto& node : block.getBody()) {
    switch (node->getType()) {
      case sun::ast::ASTNodeType::MODULE:
        forEachGlobalDeclaration(
            static_cast<const sun::ast::ModuleAST&>(*node).getBody(), fn);
        break;
      case sun::ast::ASTNodeType::MOON_SCOPE:
        forEachGlobalDeclaration(
            static_cast<const sun::ast::MoonScopeAST&>(*node).getBody(), fn);
        break;
      case sun::ast::ASTNodeType::VARIABLE_CREATION:
        fn(static_cast<const sun::ast::VariableCreationAST&>(*node));
        break;
      default:
        break;
    }
  }
}

/** The node that declares a variable, or null when `id` is not a variable. */
inline const sun::ast::VariableCreationAST* findVariableNode(
    const DeclarationTable& declarations, DeclarationId id) {
  if (!id) return nullptr;
  const sun::ast::ExprAST* node = declarations.get(id).astNode;
  if (!node || node->getType() != sun::ast::ASTNodeType::VARIABLE_CREATION)
    return nullptr;
  return static_cast<const sun::ast::VariableCreationAST*>(node);
}

}  // namespace sun::semantic_analysis
