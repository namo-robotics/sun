// interface_definition_ast.cpp — InterfaceDefinitionAST clone implementation

#include "ast/interface_definition_ast.h"

std::unique_ptr<ExprAST> InterfaceDefinitionAST::clone() const {
  // Clone fields
  std::vector<InterfaceFieldDecl> fieldsClone;
  fieldsClone.reserve(fields.size());
  for (const auto& field : fields) {
    fieldsClone.push_back({field.name, field.type, field.location});
  }

  // Clone methods
  std::vector<InterfaceMethodDecl> methodsClone;
  methodsClone.reserve(methods.size());
  for (const auto& method : methods) {
    InterfaceMethodDecl methodClone;
    auto funcClone = method.function->clone();
    methodClone.function.reset(static_cast<FunctionAST*>(funcClone.release()));
    methodClone.hasDefaultImpl = method.hasDefaultImpl;
    methodsClone.push_back(std::move(methodClone));
  }

  auto copy = std::make_unique<InterfaceDefinitionAST>(
      name, typeParameters, std::move(fieldsClone), std::move(methodsClone));
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}
