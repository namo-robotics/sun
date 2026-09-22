#include "semantic_analysis/passes/field_initializer_preparation_pass.h"

#include "ast.h"
#include "ast/ast_children.h"
#include "semantic_analysis/field_initialization.h"

using sun::ast::ExprAST;

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

void FieldInitializerPreparationPass::run(ExprAST& root,
                                          bool skipImportedMoons) const {
  if (skipImportedMoons &&
      root.getType() == sun::ast::ASTNodeType::MOON_SCOPE &&
      !static_cast<const sun::ast::MoonScopeAST&>(root).isOwnBundle())
    return;
  if (root.getType() == sun::ast::ASTNodeType::CLASS_DEFINITION)
    prepareFieldInitializers(static_cast<sun::ast::ClassDefinitionAST&>(root));
  sun::ast::forEachChild(root, [&](const ExprAST& child) {
    run(const_cast<ExprAST&>(child), skipImportedMoons);
  });
}

void FieldInitializerPreparationPass::run(
    const std::vector<sun::ast::MoonScopeAST*>& imports) const {
  for (auto* moon : imports) run(*moon);
}

}  // namespace sun::semantic_analysis::passes
