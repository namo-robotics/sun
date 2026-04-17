// semantic_analysis/interfaces.cpp — Interface, enum support, and validation

#include "error.h"
#include "semantic_analyzer.h"

// -------------------------------------------------------------------
// Interface support
// -------------------------------------------------------------------

void SemanticAnalyzer::registerInterface(
    const std::string& name,
    std::shared_ptr<sun::InterfaceType> interfaceType) {
  interfaceTable[name] = std::move(interfaceType);
}

std::shared_ptr<sun::InterfaceType> SemanticAnalyzer::lookupInterface(
    const std::string& name) const {
  auto it = interfaceTable.find(name);
  return it != interfaceTable.end() ? it->second : nullptr;
}

// -------------------------------------------------------------------
// Generic interface support
// -------------------------------------------------------------------

void SemanticAnalyzer::registerGenericInterface(
    const std::string& name, const GenericInterfaceInfo& info) {
  genericInterfaceTable[name] = info;
}

const GenericInterfaceInfo* SemanticAnalyzer::lookupGenericInterface(
    const std::string& name) const {
  auto it = genericInterfaceTable.find(name);
  return it != genericInterfaceTable.end() ? &it->second : nullptr;
}

std::shared_ptr<sun::InterfaceType>
SemanticAnalyzer::instantiateGenericInterface(
    const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs) {
  // Generate mangled name for the specialized interface
  std::string mangledName =
      sun::Types::mangleGenericClassName(baseName, typeArgs);

  // Check if already instantiated
  auto existing = lookupInterface(mangledName);
  if (existing) {
    return existing;
  }

  // Look up the generic interface definition
  auto* genericInfo = lookupGenericInterface(baseName);

  // If not found in analyzer's table, check if it's a builtin interface
  // (IIterator<T>, IIterable<T>)
  if (!genericInfo || !genericInfo->AST) {
    // Try to find builtin interface by stripping module prefix if needed
    // e.g., "sun_IIterator" -> "IIterator"
    // Don't use getInterface() directly as it auto-creates empty interfaces
    std::string unqualifiedName = baseName;
    size_t underscorePos = baseName.find('_');
    if (underscorePos != std::string::npos) {
      unqualifiedName = baseName.substr(underscorePos + 1);
    }

    // Look up the unqualified name - builtin interfaces are registered without
    // prefix
    auto builtinInterface = typeRegistry->lookupInterface(unqualifiedName);
    if (builtinInterface && builtinInterface->isGenericDefinition()) {
      // Verify type argument count
      if (typeArgs.size() != builtinInterface->getTypeParameters().size()) {
        llvm::errs() << "Error: Generic interface '" << baseName << "' expects "
                     << builtinInterface->getTypeParameters().size()
                     << " type arguments, got " << typeArgs.size() << "\n";
        return nullptr;
      }

      // Create specialized interface from builtin
      auto specializedInterface =
          typeRegistry->getSpecializedInterface(baseName, typeArgs);

      // Push a scope for type parameter bindings
      enterScope();
      addTypeParameterBindings(builtinInterface->getTypeParameters(), typeArgs);

      // Add fields with substituted types
      for (const auto& field : builtinInterface->getFields()) {
        auto fieldType = substituteTypeParameters(field.type);
        specializedInterface->addField(field.name, fieldType);
      }

      // Add methods with substituted types
      for (const auto& method : builtinInterface->getMethods()) {
        auto returnType = substituteTypeParameters(method.returnType);
        std::vector<sun::TypePtr> paramTypes;
        for (const auto& pt : method.paramTypes) {
          paramTypes.push_back(substituteTypeParameters(pt));
        }
        specializedInterface->addMethod(method.name, returnType, paramTypes,
                                        method.hasDefaultImpl,
                                        method.typeParameters);
      }

      // Pop the scope
      exitScope();

      // Register the specialized interface
      registerInterface(mangledName, specializedInterface);

      return specializedInterface;
    }

    llvm::errs() << "Error: Unknown generic interface '" << baseName << "'\n";
    return nullptr;
  }

  // Verify type argument count matches
  if (typeArgs.size() != genericInfo->typeParameters.size()) {
    llvm::errs() << "Error: Generic interface '" << baseName << "' expects "
                 << genericInfo->typeParameters.size()
                 << " type arguments, got " << typeArgs.size() << "\n";
    return nullptr;
  }

  // Create the specialized interface type
  auto specializedInterface =
      typeRegistry->getSpecializedInterface(baseName, typeArgs);

  // Push a scope for type parameter bindings
  enterScope();
  addTypeParameterBindings(genericInfo->typeParameters, typeArgs);

  // Add fields with substituted types
  for (const auto& field : genericInfo->AST->getFields()) {
    auto fieldType = typeAnnotationToType(field.type);
    fieldType = substituteTypeParameters(fieldType);
    specializedInterface->addField(field.name, fieldType);
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
    specializedInterface->addMethod(proto.getName(), returnType, paramTypes,
                                    methodDecl.hasDefaultImpl,
                                    proto.getTypeParameters());
  }

  // Pop the scope
  exitScope();

  // Register the specialized interface
  registerInterface(mangledName, specializedInterface);

  return specializedInterface;
}

// -------------------------------------------------------------------
// Enum support
// -------------------------------------------------------------------

void SemanticAnalyzer::registerEnum(const std::string& name,
                                    std::shared_ptr<sun::EnumType> enumType) {
  enumTable[name] = std::move(enumType);
}

std::shared_ptr<sun::EnumType> SemanticAnalyzer::lookupEnum(
    const std::string& name) const {
  auto it = enumTable.find(name);
  return it != enumTable.end() ? it->second : nullptr;
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
                         ifaceRef.name + "'");
      }
      interfaceDisplayName = interfaceType->toString();
    } else {
      // Non-generic interface
      interfaceType = lookupInterface(ifaceRef.name);
      if (!interfaceType) {
        logAndThrowError("Class '" + classDef.getName() +
                         "' implements unknown interface '" + ifaceRef.name +
                         "'");
      }
    }

    // Add interface fields to class (interface fields are inherited)
    for (const auto& field : interfaceType->getFields()) {
      // Check if class already has this field
      const sun::ClassField* existingField = classType->getField(field.name);
      if (existingField) {
        // Field already declared in class - verify type matches
        if (!existingField->type->equals(*field.type)) {
          logAndThrowError("Class '" + classDef.getName() +
                           "' declares field '" + field.name + "' with type '" +
                           existingField->type->toString() +
                           "' but interface '" + interfaceDisplayName +
                           "' requires type '" + field.type->toString() + "'");
        }
        continue;
      }
      // Add interface field to class
      classType->addField(field.name, field.type);
    }
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
      interfaceDisplayName = interfaceType->toString();
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
        // Verify return type matches
        if (classMethodInfo->returnType && interfaceMethod.returnType &&
            !classMethodInfo->returnType->equals(*interfaceMethod.returnType)) {
          logAndThrowError("Class '" + classDef.getName() + "' method '" +
                           interfaceMethod.name + "' has return type '" +
                           classMethodInfo->returnType->toString() +
                           "' but interface '" + interfaceDisplayName +
                           "' requires return type '" +
                           interfaceMethod.returnType->toString() + "'");
        }
        // Verify parameter count matches
        if (classMethodInfo->paramTypes.size() !=
            interfaceMethod.paramTypes.size()) {
          logAndThrowError("Class '" + classDef.getName() + "' method '" +
                           interfaceMethod.name + "' has " +
                           std::to_string(classMethodInfo->paramTypes.size()) +
                           " parameters but interface '" +
                           interfaceDisplayName + "' requires " +
                           std::to_string(interfaceMethod.paramTypes.size()) +
                           " parameters");
        } else {
          // Verify each parameter type matches
          for (size_t i = 0; i < classMethodInfo->paramTypes.size(); ++i) {
            if (!classMethodInfo->paramTypes[i]->equals(
                    *interfaceMethod.paramTypes[i])) {
              logAndThrowError("Class '" + classDef.getName() + "' method '" +
                               interfaceMethod.name + "' parameter " +
                               std::to_string(i + 1) + " has type '" +
                               classMethodInfo->paramTypes[i]->toString() +
                               "' but interface '" + interfaceDisplayName +
                               "' requires type '" +
                               interfaceMethod.paramTypes[i]->toString() + "'");
            }
          }
        }
      } else {
        // No override found
        if (interfaceMethod.hasDefaultImpl) {
          // Add the default method to the class type so it can be found during
          // lookup (preserve generic type parameters)
          classType->addMethod(interfaceMethod.name, interfaceMethod.returnType,
                               interfaceMethod.paramTypes, false,
                               interfaceMethod.typeParameters);

          // Register the mangled method name as a function
          std::string mangledName =
              classType->getMangledMethodName(interfaceMethod.name);
          std::vector<sun::TypePtr> methodParamTypes;
          methodParamTypes.push_back(classType);  // this parameter
          for (const auto& pt : interfaceMethod.paramTypes) {
            methodParamTypes.push_back(pt);
          }
          registerFunction(mangledName,
                           {interfaceMethod.returnType, methodParamTypes, {}});
        } else {
          // Required method not implemented
          logAndThrowError("Class '" + classDef.getName() +
                           "' does not implement required method '" +
                           interfaceMethod.name + "' from interface '" +
                           interfaceDisplayName + "'");
        }
      }
    }

    // Track that this class implements this interface (use mangled name for
    // generics)
    classType->addImplementedInterface(interfaceType->getName());
  }
}
