// semantic_analysis/interfaces.cpp — Interface, enum support, and validation

#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

// -------------------------------------------------------------------
// Enum lookup (the rest of enum analysis lives in enums.cpp)
// -------------------------------------------------------------------

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
        auto argType = types_.typeAnnotationToType(typeArg);
        argType = types_.substituteTypeParameters(argType);
        typeArgs.push_back(argType);
      }

      // Instantiate the generic interface
      interfaceType =
          generics_.instantiateGenericInterface(ifaceRef.name, typeArgs);
      if (!interfaceType) {
        logAndThrowError("Class '" + classDef.getName() +
                             "' implements unknown generic interface '" +
                             ifaceRef.name + "'",
                         classDef.getLocation());
      }
      interfaceDisplayName = interfaceType->toDisplayString();
    } else {
      // Non-generic interface
      interfaceType = ctx_.lookupInterface(ifaceRef.name);
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
                  existingField->type->toDisplayString() + "' but interface '" +
                  interfaceDisplayName + "' requires type '" +
                  field.type->toDisplayString() + "'",
              classDef.getLocation());
        }
        continue;
      }
      // Add interface field to class with the interface's visibility
      classType->addField(field.name, field.type).visibility = field.visibility;
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
        auto argType = types_.typeAnnotationToType(typeArg);
        argType = types_.substituteTypeParameters(argType);
        typeArgs.push_back(argType);
      }

      interfaceType =
          generics_.instantiateGenericInterface(ifaceRef.name, typeArgs);
      if (!interfaceType) {
        // Already reported in inheritInterfaceFields
        continue;
      }
      interfaceDisplayName = interfaceType->toDisplayString();
    } else {
      interfaceType = ctx_.lookupInterface(ifaceRef.name);
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
          logSemanticError(
              "method '" + interfaceMethod.name + "' of class '" +
                  classType->getDisplayName() + "' implements const member '" +
                  interfaceDisplayName + "." + interfaceMethod.name +
                  "' and must be declared 'const function'",
              classDef.getLocation());
        }
        // Verify return type matches. A class return where the interface
        // declares an interface type it implements is accepted (IIterable's
        // iter() returns the concrete iterator), but such a method cannot be
        // dispatched through a fat pointer, so the class is not convertible
        // to this interface.
        bool returnOk =
            !classMethodInfo->returnType || !interfaceMethod.returnType ||
            classMethodInfo->returnType->equals(*interfaceMethod.returnType);
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
          logAndThrowError(
              "Class '" + classType->getDisplayName() + "' method '" +
                  interfaceMethod.name + "' has return type '" +
                  classMethodInfo->returnType->toDisplayString() +
                  "' but interface '" + interfaceDisplayName +
                  "' requires return type '" +
                  interfaceMethod.returnType->toDisplayString() + "'",
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
              logAndThrowError(
                  "Class '" + classType->getDisplayName() + "' method '" +
                      interfaceMethod.name + "' parameter " +
                      std::to_string(i + 1) + " has type '" +
                      classMethodInfo->paramTypes[i]->toDisplayString() +
                      "' but interface '" + interfaceDisplayName +
                      "' requires type '" +
                      interfaceMethod.paramTypes[i]->toDisplayString() + "'",
                  classDef.getLocation());
            }
          }
        }
      } else {
        // No override found
        if (interfaceMethod.hasDefaultImpl) {
          // Add the default method to the class type so it can be found during
          // lookup (preserve generic type parameters)
          auto& method = classType->addMethod(interfaceMethod.name,
                                              interfaceMethod.returnType,
                                              interfaceMethod.paramTypes, false,
                                              interfaceMethod.typeParameters);
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
          ctx_.registerFunctionInCurrentScope(
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
