// semantic_analysis/interfaces.cpp — Interface, enum support, and validation

#include "error.h"
#include "semantic_analyzer.h"

// -------------------------------------------------------------------
// Interface support
// -------------------------------------------------------------------

void SemanticAnalyzer::registerInterface(
    const std::string& name, std::shared_ptr<sun::InterfaceType> interfaceType,
    std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (currentScope->interfaces.contains(name)) {
    return;
  }
  // Register in current scope
  currentScope->interfaces[name] = interfaceType;
}

std::shared_ptr<sun::InterfaceType> SemanticAnalyzer::lookupInterface(
    const std::string& name) const {
  auto result = currentScope->lookupInterface(name);
  if (result) return result;

  // Check builtin interfaces in type registry (IError)
  if (typeRegistry) {
    auto builtinInterface = typeRegistry->lookupInterface(name);
    if (builtinInterface) return builtinInterface;
  }

  return nullptr;
}

// -------------------------------------------------------------------
// Generic interface support
// -------------------------------------------------------------------

void SemanticAnalyzer::registerGenericInterface(
    const std::string& name, const GenericInterfaceInfo& info,
    std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (currentScope->genericInterfaces.contains(name)) {
    return;
  }
  // Register in current scope
  auto& slot = currentScope->genericInterfaces[name];
  slot = info;
  slot.definitionScope = currentScope->shared_from_this();
}

const GenericInterfaceInfo* SemanticAnalyzer::lookupGenericInterface(
    const std::string& name) const {
  return currentScope->lookupGenericInterface(name);
}

std::shared_ptr<sun::InterfaceType>
SemanticAnalyzer::instantiateGenericInterface(
    const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs) {
  // Look up the generic interface definition first
  auto* genericInfo = lookupGenericInterface(baseName);

  // Use the AST's mangled name for generating specialized interface name
  std::string effectiveBase = (genericInfo && genericInfo->AST)
                                  ? genericInfo->AST->getMangledName()
                                  : baseName;

  // Generate mangled name for the specialized interface
  std::string mangledName =
      sun::Types::mangleGenericClassName(effectiveBase, typeArgs);

  // Check if already instantiated
  auto existing = lookupInterface(mangledName);
  if (existing) {
    return existing;
  }

  if (!genericInfo || !genericInfo->AST) {
    logAndThrowError("Unknown generic interface '" + baseName + "'");
  }

  // Verify type argument count matches
  if (typeArgs.size() != genericInfo->typeParameters.size()) {
    logAndThrowError("Generic interface '" + baseName + "' expects " +
                     std::to_string(genericInfo->typeParameters.size()) +
                     " type arguments, got " + std::to_string(typeArgs.size()));
  }

  // Create the specialized interface type
  auto specializedInterface =
      typeRegistry->getSpecializedInterface(baseName, typeArgs);
  specializedInterface->visibility = genericInfo->AST->getVisibility();
  specializedInterface->setQualifiedName(sun::QualifiedName(
      genericInfo->qualifiedName.scopePath, mangledName,
      genericInfo->qualifiedName.modulePath));

  {
    // Member annotations resolve in the interface's definition scope; the
    // result is registered in the requesting scope below
    ScopeSwitchGuard definitionScope(*this, definitionScopeOf(*genericInfo));
    // Push a scope for type parameter bindings
    enterTypeParamScope(genericInfo->typeParameters, typeArgs);

    // Add fields with substituted types
    for (const auto& field : genericInfo->AST->getFields()) {
      auto fieldType = typeAnnotationToType(field.type);
      fieldType = substituteTypeParameters(fieldType);
      specializedInterface->addField(field.name, fieldType).visibility =
          field.visibility;
    }

    // Add methods with substituted types
    for (const auto& methodDecl : genericInfo->AST->getMethods()) {
      const PrototypeAST& proto = methodDecl.function->getProto();

      // Get return type with substitution
      sun::TypePtr returnType;
      if (proto.getReturnType()) {
        returnType = typeAnnotationToType(*proto.getReturnType());
        returnType = substituteTypeParameters(returnType);
      } else {
        returnType = sun::Types::Void();
      }

      // Get parameter types with substitution
      std::vector<sun::TypePtr> paramTypes;
      for (const auto& [argName, argType] : proto.getArgs()) {
        auto paramType = typeAnnotationToType(argType);
        paramType = substituteTypeParameters(paramType);
        paramTypes.push_back(paramType);
      }

      // Add method to interface type (preserve method-level generic type
      // parameters)
      auto& method = specializedInterface->addMethod(
          proto.getName(), returnType, paramTypes, methodDecl.hasDefaultImpl,
          proto.getTypeParameters());
      method.visibility = methodVisibility(*methodDecl.function);
      method.isConst = methodDecl.isConst;
    }

    // Pop the scope
    exitScope();
  }

  // Register the specialized interface
  registerInterface(mangledName, specializedInterface);

  return specializedInterface;
}

// -------------------------------------------------------------------
// Enum lookup (the rest of enum analysis lives in enums.cpp)
// -------------------------------------------------------------------

std::shared_ptr<sun::EnumType> SemanticAnalyzer::lookupEnum(
    const std::string& name) const {
  return currentScope->lookupEnum(name);
}

// -------------------------------------------------------------------
// Interface inheritance and validation
// -------------------------------------------------------------------

void SemanticAnalyzer::inheritInterfaceFields(
    const ClassDefinitionAST& classDef,
    std::shared_ptr<sun::ClassType> classType) {
  for (const auto& ifaceRef : classDef.getImplementedInterfaces()) {
    std::shared_ptr<sun::InterfaceType> interfaceType;
    std::string interfaceDisplayName = ifaceRef.name;

    if (!ifaceRef.typeArguments.empty()) {
      // Generic interface with type arguments: IIterator<T>
      // Convert type arguments, substituting any class type parameters
      std::vector<sun::TypePtr> typeArgs;
      for (const auto& typeArg : ifaceRef.typeArguments) {
        auto argType = typeAnnotationToType(typeArg);
        argType = substituteTypeParameters(argType);
        typeArgs.push_back(argType);
      }

      // Instantiate the generic interface
      interfaceType = instantiateGenericInterface(ifaceRef.name, typeArgs);
      if (!interfaceType) {
        logAndThrowError("Class '" + classDef.getName() +
                             "' implements unknown generic interface '" +
                             ifaceRef.name + "'",
                         classDef.getLocation());
      }
      interfaceDisplayName = interfaceType->toDisplayString();
    } else {
      // Non-generic interface
      interfaceType = lookupInterface(ifaceRef.name);
      if (!interfaceType) {
        logAndThrowError("Class '" + classDef.getName() +
                             "' implements unknown interface '" +
                             ifaceRef.name + "'",
                         classDef.getLocation());
      }
    }

    // Add interface fields to class (interface fields are inherited)
    for (const auto& field : interfaceType->getFields()) {
      // Check if class already has this field
      const sun::ClassField* existingField = classType->getField(field.name);
      if (existingField) {
        // Field already declared in class - verify type matches
        if (!existingField->type->equals(*field.type)) {
          logAndThrowError(
              "Class '" + classDef.getName() + "' declares field '" +
                  field.name + "' with type '" +
                  existingField->type->toDisplayString() +
                      "' but interface '" +
                  interfaceDisplayName + "' requires type '" +
                  field.type->toDisplayString() + "'",
              classDef.getLocation());
        }
        continue;
      }
      // Add interface field to class with the interface's visibility
      classType->addField(field.name, field.type).visibility =
          field.visibility;
    }

    // Record the implementation now (conformance is validated after the
    // class body is analyzed) so `throw`/interface conversions on this class
    // work in any body analyzed before its definition is reached
    classType->addImplementedInterface(interfaceType->getName());
  }
}

void SemanticAnalyzer::validateInterfaceImplementation(
    const ClassDefinitionAST& classDef,
    std::shared_ptr<sun::ClassType> classType) {
  for (const auto& ifaceRef : classDef.getImplementedInterfaces()) {
    std::shared_ptr<sun::InterfaceType> interfaceType;
    std::string interfaceDisplayName = ifaceRef.name;

    if (!ifaceRef.typeArguments.empty()) {
      // Generic interface with type arguments
      std::vector<sun::TypePtr> typeArgs;
      for (const auto& typeArg : ifaceRef.typeArguments) {
        auto argType = typeAnnotationToType(typeArg);
        argType = substituteTypeParameters(argType);
        typeArgs.push_back(argType);
      }

      interfaceType = instantiateGenericInterface(ifaceRef.name, typeArgs);
      if (!interfaceType) {
        // Already reported in inheritInterfaceFields
        continue;
      }
      interfaceDisplayName = interfaceType->toDisplayString();
    } else {
      interfaceType = lookupInterface(ifaceRef.name);
      if (!interfaceType) {
        // Already reported in inheritInterfaceFields
        continue;
      }
    }

    // Check that class implements all required methods and add default methods
    for (const auto& interfaceMethod : interfaceType->getMethods()) {
      // Check if class already has a method with this name
      const sun::ClassMethod* classMethodInfo = nullptr;
      for (const auto& classMethod : classDef.getMethods()) {
        if (classMethod.function->getProto().getName() ==
            interfaceMethod.name) {
          // Found override - get the method info from the class type
          classMethodInfo = classType->getMethod(interfaceMethod.name);
          break;
        }
      }

      if (classMethodInfo) {
        // A public interface member is reachable through the interface, so
        // the implementing method must be public too
        if (interfaceMethod.visibility == sun::Visibility::Public &&
            classMethodInfo->visibility != sun::Visibility::Public) {
          logSemanticError("method '" + interfaceMethod.name + "' of class '" +
                               classType->getDisplayName() +
                               "' implements public member '" +
                               interfaceDisplayName + "." +
                               interfaceMethod.name + "' and must be public",
                           classDef.getLocation());
        }
        // A const interface member may be called on a constant receiver, so
        // the implementing method must promise the same
        if (interfaceMethod.isConst && !classMethodInfo->isConst) {
          logSemanticError("method '" + interfaceMethod.name + "' of class '" +
                               classType->getDisplayName() +
                               "' implements const member '" +
                               interfaceDisplayName + "." +
                               interfaceMethod.name +
                               "' and must be declared 'const function'",
                           classDef.getLocation());
        }
        // Verify return type matches. A class return where the interface
        // declares an interface type it implements is accepted (IIterable's
        // iter() returns the concrete iterator), but such a method cannot be
        // dispatched through a fat pointer, so the class is not convertible
        // to this interface.
        bool returnOk = !classMethodInfo->returnType ||
                        !interfaceMethod.returnType ||
                        classMethodInfo->returnType->equals(
                            *interfaceMethod.returnType);
        if (!returnOk && interfaceMethod.returnType->isInterface() &&
            classMethodInfo->returnType->isClass()) {
          auto* required = static_cast<const sun::InterfaceType*>(
              interfaceMethod.returnType.get());
          auto* returned = static_cast<const sun::ClassType*>(
              classMethodInfo->returnType.get());
          if (returned->implementsInterface(required->getName())) {
            returnOk = true;
            classType->markStaticOnlyInterface(interfaceType->getName());
          }
        }
        if (!returnOk) {
          logAndThrowError("Class '" + classType->getDisplayName() + "' method '" +
                               interfaceMethod.name + "' has return type '" +
                               classMethodInfo->returnType
                                   ->toDisplayString() +
                               "' but interface '" + interfaceDisplayName +
                               "' requires return type '" +
                               interfaceMethod.returnType
                                   ->toDisplayString() +
                               "'",
                           classDef.getLocation());
        }
        // Verify parameter count matches
        if (classMethodInfo->paramTypes.size() !=
            interfaceMethod.paramTypes.size()) {
          logAndThrowError(
              "Class '" + classType->getDisplayName() + "' method '" +
                  interfaceMethod.name + "' has " +
                  std::to_string(classMethodInfo->paramTypes.size()) +
                  " parameters but interface '" + interfaceDisplayName +
                  "' requires " +
                  std::to_string(interfaceMethod.paramTypes.size()) +
                  " parameters",
              classDef.getLocation());
        } else {
          // Verify each parameter type matches
          for (size_t i = 0; i < classMethodInfo->paramTypes.size(); ++i) {
            if (!classMethodInfo->paramTypes[i]->equals(
                    *interfaceMethod.paramTypes[i])) {
              logAndThrowError("Class '" + classType->getDisplayName() + "' method '" +
                                   interfaceMethod.name + "' parameter " +
                                   std::to_string(i + 1) + " has type '" +
                                   classMethodInfo->paramTypes[i]
                                       ->toDisplayString() +
                                   "' but interface '" + interfaceDisplayName +
                                   "' requires type '" +
                                   interfaceMethod.paramTypes[i]
                                       ->toDisplayString() +
                                   "'",
                               classDef.getLocation());
            }
          }
        }
      } else {
        // No override found
        if (interfaceMethod.hasDefaultImpl) {
          // Add the default method to the class type so it can be found during
          // lookup (preserve generic type parameters)
          auto& method = classType->addMethod(
              interfaceMethod.name, interfaceMethod.returnType,
              interfaceMethod.paramTypes, false, interfaceMethod.typeParameters);
          method.visibility = interfaceMethod.visibility;
          method.isConst = interfaceMethod.isConst;

          // Register the mangled method name as a function
          std::string mangledName =
              classType->getMangledMethodName(interfaceMethod.name);
          std::vector<sun::TypePtr> methodParamTypes;
          methodParamTypes.push_back(classType);  // this parameter
          for (const auto& pt : interfaceMethod.paramTypes) {
            methodParamTypes.push_back(pt);
          }
          registernFunctionInCurrentScope(
              mangledName, {interfaceMethod.returnType, methodParamTypes, {}});
        } else {
          // Required method not implemented
          logAndThrowError("Class '" + classType->getDisplayName() +
                               "' does not implement required method '" +
                               interfaceMethod.name + "' from interface '" +
                               interfaceDisplayName + "'",
                           classDef.getLocation());
        }
      }
    }

    // Track that this class implements this interface (use mangled name for
    // generics)
    classType->addImplementedInterface(interfaceType->getName());
  }
}
