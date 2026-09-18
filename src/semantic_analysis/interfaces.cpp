// semantic_analysis/interfaces.cpp — Interface, enum support, and validation

#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

using sun::semantic_analysis::ClassType;
using sun::semantic_analysis::InterfaceType;
using sun::semantic_analysis::TypePtr;

using sun::support::logAndThrowError;
using sun::support::logSemanticError;

namespace sun::semantic_analysis {

// -------------------------------------------------------------------
// Enum lookup (the rest of enum analysis lives in enums.cpp)
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Interface inheritance and validation
// -------------------------------------------------------------------

void SemanticAnalyzer::inheritInterfaceFields(
    const sun::ast::ClassDefinitionAST& classDef,
    std::shared_ptr<ClassType> classType) {
  for (const auto& ifaceRef : classDef.getImplementedInterfaces()) {
    auto interfaceType = std::dynamic_pointer_cast<InterfaceType>(
        types_.typeAnnotationToType(ifaceRef.toAnnotation()));
    if (!interfaceType)
      logAndThrowError("Class '" + classDef.getName() +
                           "' implements unknown interface '" + ifaceRef.name +
                           "'",
                       classDef.getLocation());
    std::string interfaceDisplayName = interfaceType->toDisplayString();

    // Add interface fields to class (interface fields are inherited)
    for (const auto& field : interfaceType->getFields()) {
      // Check if class already has this field
      const sun::semantic_analysis::ClassField* existingField =
          classType->getField(field.name);
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
      const auto owner = classType->getDeclarationId();
      auto id = ctx_.types()->declarations.add(
          sun::semantic_analysis::DeclarationKind::Field, field.name, owner,
          ctx_.types()->declarations.get(owner).module, {},
          field.declarationId);
      classType->addField(field.name, field.type, id).visibility =
          field.visibility;
    }

    // Record the implementation now (conformance is validated after the
    // class body is analyzed) so `throw`/interface conversions on this class
    // work in any body analyzed before its definition is reached
    classType->addImplementedInterface(*interfaceType);
  }
}

void SemanticAnalyzer::validateInterfaceImplementation(
    const sun::ast::ClassDefinitionAST& classDef,
    std::shared_ptr<ClassType> classType) {
  for (const auto& ifaceRef : classDef.getImplementedInterfaces()) {
    auto interfaceType = std::dynamic_pointer_cast<InterfaceType>(
        types_.typeAnnotationToType(ifaceRef.toAnnotation()));
    if (!interfaceType) continue;
    std::string interfaceDisplayName = interfaceType->toDisplayString();

    // Check that class implements all required methods and add default methods
    for (const auto& interfaceMethod : interfaceType->getMethods()) {
      const sun::semantic_analysis::ClassMethod* classMethodInfo = nullptr;
      auto requiredReturnType = interfaceMethod.returnType;
      auto requiredParamTypes = interfaceMethod.paramTypes;
      for (const auto& classMethod : classDef.getMethods()) {
        const auto& proto = classMethod.function->getProto();
        if (proto.getName() != interfaceMethod.name ||
            proto.getTypeParameters().size() !=
                interfaceMethod.typeParameters.size())
          continue;

        requiredReturnType = interfaceMethod.returnType;
        requiredParamTypes = interfaceMethod.paramTypes;
        // Corresponding generic method parameters have different identities.
        // Compare the requirement after binding its parameters to this
        // candidate's.
        SemanticContext::ScopeSwitchGuard candidateScope(ctx_, ctx_.scope());
        if (!interfaceMethod.typeParameters.empty()) {
          std::vector<TypePtr> parameters;
          for (size_t i = 0; i < proto.getTypeParameters().size(); ++i)
            parameters.push_back(proto.getTypeParameters()[i].toSunType(
                ctx_.types()->declarations,
                proto.declarationIdentity().typeParameters.at(i)));
          ctx_.enterTypeParamScope(interfaceMethod.typeParameters, parameters);
          requiredReturnType =
              types_.substituteTypeParameters(requiredReturnType);
          for (auto& type : requiredParamTypes)
            type = types_.substituteTypeParameters(type);
        }
        auto* candidate = classType->getMethodForArgs(interfaceMethod.name,
                                                      requiredParamTypes);
        if (candidate && candidate->declarationId == proto.getDeclarationId()) {
          classMethodInfo = candidate;
          break;
        }
      }

      if (classMethodInfo) {
        classType->bindInterfaceMethod(interfaceMethod.declarationId,
                                       classMethodInfo->declarationId);
        // A public interface member is reachable through the interface, so
        // the implementing method must be public too
        if (interfaceMethod.visibility ==
                sun::semantic_analysis::Visibility::Public &&
            classMethodInfo->visibility !=
                sun::semantic_analysis::Visibility::Public) {
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
                  "' and must be declared 'const method'",
              classDef.getLocation());
        }
        if (classMethodInfo->isUnsafe && !interfaceMethod.isUnsafe) {
          logSemanticError("unsafe method '" + interfaceMethod.name +
                               "' cannot implement a safe interface method",
                           classDef.getLocation());
        }
        // Verify return type matches. A class return where the interface
        // declares an interface type it implements is accepted (IIterable's
        // iter() returns the concrete iterator), but such a method cannot be
        // dispatched through a fat pointer, so the class is not convertible
        // to this interface.
        bool returnOk =
            !classMethodInfo->returnType || !requiredReturnType ||
            classMethodInfo->returnType->equals(*requiredReturnType);
        if (!returnOk && requiredReturnType->isInterface() &&
            classMethodInfo->returnType->isClass()) {
          auto* required =
              static_cast<const InterfaceType*>(requiredReturnType.get());
          auto* returned =
              static_cast<const ClassType*>(classMethodInfo->returnType.get());
          if (returned->implementsInterface(*required)) {
            returnOk = true;
            classType->markStaticOnlyInterface(*interfaceType);
          }
        }
        if (!returnOk) {
          logAndThrowError("Class '" + classType->getDisplayName() +
                               "' method '" + interfaceMethod.name +
                               "' has return type '" +
                               classMethodInfo->returnType->toDisplayString() +
                               "' but interface '" + interfaceDisplayName +
                               "' requires return type '" +
                               requiredReturnType->toDisplayString() + "'",
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
          // Lifetime relations are part of the contract: a caller
          // dispatching through the interface sees only the interface's
          // names, so the implementation must promise the same ties.
          // Names match verbatim - 'this is 'this, and declared names
          // match by spelling.
          auto lifetimeContractOf = [](const TypePtr& t) -> std::string {
            if (auto* lt = sun::codegen::support::tryGetType<
                    sun::semantic_analysis::LambdaType>(t)) {
              return lt->getLifetimeName();
            }
            if (auto* rt = sun::codegen::support::tryGetType<
                    sun::semantic_analysis::ReferenceType>(t)) {
              std::string contract = rt->getLifetimeName();
              for (const auto& applied : rt->getClassLifetimeArgs()) {
                contract += "<" + applied + ">";
              }
              return contract;
            }
            return "";
          };
          // Verify each parameter type matches
          for (size_t i = 0; i < classMethodInfo->paramTypes.size(); ++i) {
            if (!classMethodInfo->paramTypes[i]->equals(
                    *requiredParamTypes[i])) {
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
            if (lifetimeContractOf(classMethodInfo->paramTypes[i]) !=
                lifetimeContractOf(interfaceMethod.paramTypes[i])) {
              logAndThrowError(
                  "Class '" + classType->getDisplayName() + "' method '" +
                      interfaceMethod.name + "' parameter " +
                      std::to_string(i + 1) +
                      " does not declare the lifetime the interface '" +
                      interfaceDisplayName +
                      "' requires - the names must "
                      "match the interface's exactly",
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
          method.declarationId = ctx_.types()->declarations.add(
              sun::semantic_analysis::DeclarationKind::Function,
              interfaceMethod.name, classType->getDeclarationId(),
              ctx_.types()
                  ->declarations.get(classType->getDeclarationId())
                  .module,
              {}, interfaceMethod.declarationId);
          if (method.name == "deinit")
            classType->deinitializer = method.declarationId;
          method.defaultImplementation = interfaceMethod.declarationId;
          classType->bindInterfaceMethod(interfaceMethod.declarationId,
                                         method.declarationId);
          method.visibility = interfaceMethod.visibility;
          method.isConst = interfaceMethod.isConst;
          method.isUnsafe = interfaceMethod.isUnsafe;

          // Register the source method name as a function
          std::string methodNameForScope = interfaceMethod.name;
          std::vector<TypePtr> methodParamTypes;
          methodParamTypes.push_back(classType);  // this parameter
          for (const auto& pt : interfaceMethod.paramTypes) {
            methodParamTypes.push_back(pt);
          }
          FunctionInfo methodInfo{
              interfaceMethod.returnType, methodParamTypes, {}};
          methodInfo.declarationId = method.declarationId;
          ctx_.currentScope().declareFunction(methodNameForScope, methodInfo,
                                              ctx_.currentLocation());
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

    // Retain the resolved interface for conformance and conversion checks.
    classType->addImplementedInterface(*interfaceType);
  }
}

}  // namespace sun::semantic_analysis
