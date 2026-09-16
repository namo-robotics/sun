#include "semantic_analysis/field_initializer_preparation_pass.h"

#include "ast.h"
#include "ast/ast_children.h"
#include "semantic_analysis/field_initialization.h"

namespace sun {

void FieldInitializerPreparationPass::run(ExprAST& root) const {
  if (root.getType() == ASTNodeType::CLASS_DEFINITION)
    prepareFieldInitializers(static_cast<ClassDefinitionAST&>(root));
  forEachChild(root,
               [&](const ExprAST& child) { run(const_cast<ExprAST&>(child)); });
}

}  // namespace sun
