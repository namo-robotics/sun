#include "semantic_analysis/field_initializer_preparation_pass.h"

#include "ast.h"
#include "ast/ast_children.h"
#include "semantic_analysis/field_initialization.h"

using sun::ast::ExprAST;

namespace sun::semantic_analysis {

void FieldInitializerPreparationPass::run(ExprAST& root) const {
  if (root.getType() == sun::ast::ASTNodeType::CLASS_DEFINITION)
    prepareFieldInitializers(static_cast<sun::ast::ClassDefinitionAST&>(root));
  sun::ast::forEachChild(
      root, [&](const ExprAST& child) { run(const_cast<ExprAST&>(child)); });
}

}  // namespace sun::semantic_analysis
