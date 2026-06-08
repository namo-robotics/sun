// enum_definition_ast.cpp — EnumDefinitionAST clone implementation

#include "ast/enum_definition_ast.h"

std::unique_ptr<ExprAST> EnumDefinitionAST::clone() const {
  // Clone variants
  std::vector<EnumVariantDecl> variantsClone;
  variantsClone.reserve(variants.size());
  for (const auto& variant : variants) {
    variantsClone.push_back({variant.name, variant.value, variant.location});
  }

  auto copy =
      std::make_unique<EnumDefinitionAST>(name, std::move(variantsClone));
  cloneBase(*copy);
  return copy;
}
