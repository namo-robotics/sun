// class_definition_ast.cpp — ClassDefinitionAST clone implementation

#include "ast/class_definition_ast.h"

std::unique_ptr<ExprAST> ClassDefinitionAST::clone() const {
  // Clone fields
  std::vector<ClassFieldDecl> fieldsClone;
  fieldsClone.reserve(fields.size());
  for (const auto& field : fields) {
    fieldsClone.push_back({field.name, field.type, field.location});
  }

  // Clone methods
  std::vector<ClassMethodDecl> methodsClone;
  methodsClone.reserve(methods.size());
  for (const auto& method : methods) {
    ClassMethodDecl methodClone;
    auto funcClone = method.function->clone();
    methodClone.function.reset(static_cast<FunctionAST*>(funcClone.release()));
    methodClone.isConstructor = method.isConstructor;
    methodsClone.push_back(std::move(methodClone));
  }

  // Clone implemented interfaces
  std::vector<ImplementedInterfaceAST> interfacesClone;
  interfacesClone.reserve(implementedInterfaces.size());
  for (const auto& iface : implementedInterfaces) {
    ImplementedInterfaceAST ifaceClone;
    ifaceClone.name = iface.name;
    for (const auto& ta : iface.typeArguments) {
      ifaceClone.typeArguments.push_back(ta);  // TypeAnnotation has copy ctor
    }
    interfacesClone.push_back(std::move(ifaceClone));
  }

  auto copy = std::make_unique<ClassDefinitionAST>(
      name, typeParameters, std::move(interfacesClone), std::move(fieldsClone),
      std::move(methodsClone));
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}
