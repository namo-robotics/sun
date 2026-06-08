// prototype_ast.cpp — PrototypeAST clone implementation

#include "ast/prototype_ast.h"

std::unique_ptr<PrototypeAST> PrototypeAST::clone() const {
  // Clone args
  std::vector<std::pair<std::string, TypeAnnotation>> argsClone;
  argsClone.reserve(args.size());
  for (const auto& [argName, argType] : args) {
    argsClone.emplace_back(argName, argType);  // TypeAnnotation has copy ctor
  }

  // Clone return type
  std::optional<TypeAnnotation> retTypeClone;
  if (returnType) {
    retTypeClone = *returnType;
  }

  // Clone variadic constraint
  std::optional<TypeAnnotation> variadicConstraintClone;
  if (variadicConstraint_) {
    variadicConstraintClone = *variadicConstraint_;
  }

  auto copy = std::make_unique<PrototypeAST>(
      Name, std::move(argsClone), std::move(retTypeClone), typeParameters,
      variadicParamName_, std::move(variadicConstraintClone));
  copy->setCaptures(captures);

  // Copy analysis data
  if (hasAnalysis()) {
    const auto* a = getAnalysis();
    if (a->resolvedParamTypesSet) {
      copy->setResolvedParamTypes(a->resolvedParamTypes);
    }
    if (a->resolvedReturnType) {
      copy->setResolvedReturnType(a->resolvedReturnType);
    }
    if (!a->resolvedVariadicTypes.empty()) {
      copy->setResolvedVariadicTypes(a->resolvedVariadicTypes);
    }
    if (!a->typeBindings.empty()) {
      copy->setTypeBindings(a->typeBindings);
    }
    if (!a->qualifiedName.empty()) {
      copy->setQualifiedName(a->qualifiedName);
    }
  }

  return copy;
}
