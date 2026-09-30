/** Resolves interface contracts, default bodies, and class conformance. */

#include "semantic_analysis/interface_analysis/interface_analyzer.h"

#include <algorithm>

#include "ast/ast_utils.h"
#include "semantic_analysis/call_analysis/method_signature_set.h"
#include "semantic_analysis/declaration_analysis/declaration_rules.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_analysis/type_traits.h"
#include "support/error.h"

using sun::ast::MemberAccessAST;
using sun::types::ClassType;
using sun::types::InterfaceField;
using sun::types::InterfaceMethod;
using sun::types::InterfaceType;
using sun::types::TypePtr;
using sun::types::Types;

using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Keeps lifetime-contract utilities local to interface checking. */
namespace {
/** Rebinds inherited borrow metadata without changing the parent's types. */
TypePtr rebindInterfaceLifetimes(
    const TypePtr& type, const std::map<std::string, std::string>& bindings) {
  /** Maps a declared parent lifetime to the child's supplied lifetime. */
  auto name = [&](const std::string& value) {
    auto found = bindings.find(value);
    return found == bindings.end() ? value : found->second;
  };
  if (auto* reference =
          dynamic_cast<const sun::types::ReferenceType*>(type.get())) {
    auto result = std::make_shared<sun::types::ReferenceType>(
        rebindInterfaceLifetimes(reference->getReferencedType(), bindings),
        reference->isMutable());
    result->setLifetimeName(name(reference->getLifetimeName()));
    auto arguments = reference->getClassLifetimeArgs();
    for (auto& argument : arguments) argument = name(argument);
    result->setClassLifetimeArgs(std::move(arguments));
    return result;
  }
  if (auto* lambda = dynamic_cast<const sun::types::LambdaType*>(type.get())) {
    auto parameters = lambda->getParamTypes();
    for (auto& parameter : parameters)
      parameter = rebindInterfaceLifetimes(parameter, bindings);
    auto result = std::make_shared<sun::types::LambdaType>(
        rebindInterfaceLifetimes(lambda->getReturnType(), bindings),
        std::move(parameters), lambda->canThrow(), lambda->requiresUnsafe());
    result->setHasRefCaptures(lambda->hasRefCaptures());
    result->setLifetimeName(name(lambda->getLifetimeName()));
    return result;
  }
  return type;
}

/** Compares borrow relationships that ordinary nominal equality omits. */
bool sameInterfaceLifetimes(const TypePtr& left, const TypePtr& right) {
  if (auto* a = dynamic_cast<const sun::types::ReferenceType*>(left.get())) {
    auto* b = dynamic_cast<const sun::types::ReferenceType*>(right.get());
    return b && a->getLifetimeName() == b->getLifetimeName() &&
           a->getClassLifetimeArgs() == b->getClassLifetimeArgs() &&
           sameInterfaceLifetimes(a->getReferencedType(),
                                  b->getReferencedType());
  }
  if (auto* a = dynamic_cast<const sun::types::LambdaType*>(left.get())) {
    auto* b = dynamic_cast<const sun::types::LambdaType*>(right.get());
    if (!b || a->getLifetimeName() != b->getLifetimeName() ||
        a->getParamTypes().size() != b->getParamTypes().size() ||
        !sameInterfaceLifetimes(a->getReturnType(), b->getReturnType()))
      return false;
    for (size_t i = 0; i < a->getParamTypes().size(); ++i)
      if (!sameInterfaceLifetimes(a->getParamTypes()[i], b->getParamTypes()[i]))
        return false;
  }
  return true;
}
}  // namespace

void InterfaceAnalyzer::ensureClassInterfaces(DeclarationId declaration) {
  if (preparedTypes_.contains(declaration) ||
      preparingTypes_.contains(declaration))
    return;
  auto found = ctx_.classDefinitions.find(declaration);
  if (found == ctx_.classDefinitions.end()) return;
  preparingTypes_.insert(declaration);
  SemanticContext::ScopeSwitchGuard scope(ctx_, found->second.scope);
  SemanticContext::SourceFileGuard file(ctx_,
                                        found->second.node->getSourceFileId());
  try {
    auto classType = ctx_.types()->getClass(declaration);
    for (const auto& reference :
         found->second.node->getImplementedInterfaces()) {
      auto interfaceType = std::dynamic_pointer_cast<InterfaceType>(
          resolver_.typeAnnotationToType(reference.toAnnotation(), true));
      if (!interfaceType)
        logAndThrowError("Class '" + found->second.node->getName() +
                             "' implements unknown interface '" +
                             reference.name + "'",
                         found->second.node->getLocation());
      classType->addImplementedInterface(*interfaceType);
    }
    preparedTypes_.insert(declaration);
    preparingTypes_.erase(declaration);
  } catch (...) {
    preparingTypes_.erase(declaration);
    throw;
  }
}

void InterfaceAnalyzer::ensureInterfaceShape(DeclarationId declaration) {
  if (preparedTypes_.contains(declaration) ||
      preparingTypes_.contains(declaration))
    return;
  auto found = ctx_.interfaceDefinitions.find(declaration);
  if (found == ctx_.interfaceDefinitions.end()) return;
  preparingTypes_.insert(declaration);
  SemanticContext::ScopeSwitchGuard scope(ctx_, found->second.scope);
  SemanticContext::SourceFileGuard file(ctx_,
                                        found->second.node->getSourceFileId());
  try {
    prepareInterfaceShape(*found->second.node);
    preparedTypes_.insert(declaration);
    preparingTypes_.erase(declaration);
  } catch (...) {
    preparingTypes_.erase(declaration);
    throw;
  }
}

void InterfaceAnalyzer::mergeInterfaceParent(
    InterfaceType& interfaceType,
    const sun::ast::InterfaceDefinitionAST& definition) {
  if (!definition.getParent()) return;
  const auto root = definition.getDeclarationId();
  if (!resolvingInterfaceParents_.insert(root).second)
    logAndThrowError(
        "Interface inheritance cycle involving '" + definition.getName() + "'",
        definition.getLocation());
  std::shared_ptr<InterfaceType> parent;
  try {
    parent = std::dynamic_pointer_cast<InterfaceType>(
        resolver_.typeAnnotationToType(*definition.getParent(), true));
    if (!parent || parent->isGenericDefinition())
      logAndThrowError(
          "Interface parent must be a fully applied interface type",
          definition.getParent()->span);
    if (parent->getDeclarationId() == interfaceType.getDeclarationId() ||
        preparingTypes_.contains(parent->getDeclarationId()) ||
        parent->extendsInterface(interfaceType))
      logAndThrowError("Interface inheritance cycle involving '" +
                           definition.getName() + "'",
                       definition.getLocation());
  } catch (...) {
    resolvingInterfaceParents_.erase(root);
    throw;
  }
  resolvingInterfaceParents_.erase(root);
  const auto& lifetimeArguments = definition.getParent()->lifetimeArguments;
  if (lifetimeArguments.size() != parent->getLifetimeParams().size())
    logAndThrowError(
        "Interface parent has the wrong number of lifetime arguments",
        definition.getParent()->span);
  std::map<std::string, std::string> lifetimeBindings;
  for (size_t i = 0; i < lifetimeArguments.size(); ++i)
    lifetimeBindings.emplace(parent->getLifetimeParams()[i],
                             lifetimeArguments[i]);
  auto fields = parent->getFields();
  for (auto& field : fields)
    field.type = rebindInterfaceLifetimes(field.type, lifetimeBindings);
  for (const auto& field : interfaceType.getFields()) {
    if (parent->getField(field.name))
      logAndThrowError(
          "Interface field '" + field.name + "' redeclares an inherited field",
          definition.getLocation());
    fields.push_back(field);
  }
  auto methods = parent->getMethods();
  for (auto& method : methods) {
    method.returnType =
        rebindInterfaceLifetimes(method.returnType, lifetimeBindings);
    for (auto& parameter : method.paramTypes)
      parameter = rebindInterfaceLifetimes(parameter, lifetimeBindings);
  }
  for (const auto& local : interfaceType.getMethods()) {
    bool replaced = false;
    for (auto& inherited : methods) {
      if (local.name != inherited.name ||
          local.typeParameters.size() != inherited.typeParameters.size() ||
          local.paramTypes.size() != inherited.paramTypes.size())
        continue;
      SemanticContext::ScopeSwitchGuard methodScope(ctx_, ctx_.scope());
      if (!inherited.typeParameters.empty())
        ctx_.enterTypeParamScope(inherited.typeParameters,
                                 local.genericArguments);
      bool sameParameters = true;
      for (size_t i = 0; i < local.paramTypes.size(); ++i)
        sameParameters &= local.paramTypes[i]->equals(
            *resolver_.substituteTypeParameters(inherited.paramTypes[i]));
      if (!sameParameters) continue;
      bool sameContract =
          sameInterfaceLifetimes(local.returnType, inherited.returnType);
      for (size_t i = 0; i < local.paramTypes.size(); ++i)
        sameContract &= sameInterfaceLifetimes(local.paramTypes[i],
                                               inherited.paramTypes[i]);
      for (size_t i = 0; i < local.genericArguments.size(); ++i) {
        const auto& a = local.genericConstraints.at(i);
        const auto& b = inherited.genericConstraints.at(i);
        const bool sameTrait = a && b && a->isTypeParameter() &&
                               b->isTypeParameter() &&
                               type_analysis::isTypeTrait(a->toString()) &&
                               a->toString() == b->toString();
        sameContract &=
            (!a && !b) || sameTrait ||
            (a && b && a->equals(*resolver_.substituteTypeParameters(b)));
      }
      if (!sameContract ||
          !local.returnType->equals(
              *resolver_.substituteTypeParameters(inherited.returnType)) ||
          local.visibility != inherited.visibility ||
          local.isConst != inherited.isConst ||
          local.isUnsafe != inherited.isUnsafe ||
          (local.canThrow && !inherited.canThrow))
        logAndThrowError("Interface method '" + local.name +
                             "' changes its inherited contract",
                         definition.getLocation());
      ctx_.requireAccessible(methodRef(*parent, inherited),
                             definition.getLocation());
      auto aliases = inherited.inheritedDeclarations;
      aliases.push_back(inherited.declarationId);
      inherited = local;
      inherited.inheritedDeclarations = std::move(aliases);
      replaced = true;
      break;
    }
    if (!replaced) methods.push_back(local);
  }
  interfaceType.setInheritedShape(parent, std::move(fields),
                                  std::move(methods));
}

void InterfaceAnalyzer::inheritInterfaceFields(
    const sun::ast::ClassDefinitionAST& classDef,
    std::shared_ptr<ClassType> classType) {
  for (const auto& ifaceRef : classDef.getImplementedInterfaces()) {
    auto interfaceType = std::dynamic_pointer_cast<InterfaceType>(
        resolver_.typeAnnotationToType(ifaceRef.toAnnotation(), true));
    if (!interfaceType)
      logAndThrowError("Class '" + classDef.getName() +
                           "' implements unknown interface '" + ifaceRef.name +
                           "'",
                       classDef.getLocation());
    std::string interfaceDisplayName = interfaceType->toDisplayString();

    // Add interface fields to class (interface fields are inherited)
    for (const auto& field : interfaceType->getFields()) {
      // Check if class already has this field
      const sun::types::ClassField* existingField =
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
      auto id = ctx_.results().declarations.add(
          sun::semantic_analysis::DeclarationKind::Field, field.name, owner,
          ctx_.results().declarations.get(field.declarationId).module, {},
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

void InterfaceAnalyzer::validateInterfaceImplementation(
    const sun::ast::ClassDefinitionAST& classDef,
    std::shared_ptr<ClassType> classType) {
  std::vector<std::shared_ptr<InterfaceType>> interfaces;
  for (const auto& reference : classDef.getImplementedInterfaces()) {
    auto type = std::dynamic_pointer_cast<InterfaceType>(
        resolver_.typeAnnotationToType(reference.toAnnotation(), true));
    if (type) interfaces.push_back(type);
  }
  for (const auto& interfaceType : interfaces) {
    bool redundant = false;
    for (const auto& other : interfaces)
      if (!other->equals(*interfaceType) &&
          other->extendsInterface(*interfaceType))
        redundant = true;
    if (redundant) continue;
    std::string interfaceDisplayName = interfaceType->toDisplayString();

    // Check that class implements all required methods and add default methods
    for (const auto& interfaceMethod : interfaceType->getMethods()) {
      const sun::types::ClassMethod* classMethodInfo = nullptr;
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
                ctx_.results().declarations,
                proto.declarationIdentity().typeParameters.at(i)));
          ctx_.enterTypeParamScope(interfaceMethod.typeParameters, parameters);
          requiredReturnType =
              resolver_.substituteTypeParameters(requiredReturnType);
          for (auto& type : requiredParamTypes)
            type = resolver_.substituteTypeParameters(type);
        }
        auto* candidate = classType->getMethodForArgs(interfaceMethod.name,
                                                      requiredParamTypes);
        if (candidate && candidate->declarationId == proto.getDeclarationId()) {
          classMethodInfo = candidate;
          break;
        }
      }

      if (!classMethodInfo) {
        auto* previous = classType->getMethodForArgs(interfaceMethod.name,
                                                     requiredParamTypes);
        if (previous && previous->defaultImplementation) {
          if (interfaceMethod.hasDefaultImpl &&
              previous->defaultImplementation != interfaceMethod.declarationId)
            logAndThrowError("Conflicting interface defaults for method '" +
                                 interfaceMethod.name +
                                 "'; implement it explicitly",
                             classDef.getLocation());
          classMethodInfo = previous;
        }
      }
      if (classMethodInfo && classMethodInfo->defaultImplementation &&
          interfaceMethod.hasDefaultImpl &&
          classMethodInfo->defaultImplementation !=
              interfaceMethod.declarationId)
        logAndThrowError("Conflicting interface defaults for method '" +
                             interfaceMethod.name +
                             "'; implement it explicitly",
                         classDef.getLocation());
      if (classMethodInfo) {
        classType->bindInterfaceMethod(interfaceMethod.declarationId,
                                       classMethodInfo->declarationId);
        for (auto inherited : interfaceMethod.inheritedDeclarations)
          classType->bindInterfaceMethod(inherited,
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
        if (classMethodInfo->canThrow && !interfaceMethod.canThrow) {
          logSemanticError(
              "throwing method '" + interfaceMethod.name +
                  "' cannot implement a non-throwing interface method",
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
            for (auto ancestor = interfaceType->getParent(); ancestor;
                 ancestor = ancestor->getParent()) {
              for (const auto& requirement : ancestor->getMethods()) {
                if (requirement.declarationId ==
                        interfaceMethod.declarationId ||
                    std::find(interfaceMethod.inheritedDeclarations.begin(),
                              interfaceMethod.inheritedDeclarations.end(),
                              requirement.declarationId) !=
                        interfaceMethod.inheritedDeclarations.end())
                  classType->markStaticOnlyInterface(*ancestor);
              }
            }
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
          /** Extracts the borrow lifetime contract required of an override. */
          auto lifetimeContractOf = [](const TypePtr& t) -> std::string {
            if (auto* lt =
                    sun::codegen::support::tryGetType<sun::types::LambdaType>(
                        t)) {
              return lt->getLifetimeName();
            }
            if (auto* rt = sun::codegen::support::tryGetType<
                    sun::types::ReferenceType>(t)) {
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
          method.declarationId = ctx_.results().declarations.add(
              sun::semantic_analysis::DeclarationKind::Function,
              interfaceMethod.name, classType->getDeclarationId(),
              ctx_.results()
                  .declarations.get(classType->getDeclarationId())
                  .module,
              {}, interfaceMethod.declarationId);
          if (method.name == "deinit")
            classType->deinitializer = method.declarationId;
          method.defaultImplementation = interfaceMethod.declarationId;
          // Defaults are checked against the concrete receiver. This keeps
          // inherited field offsets and calls to overridden methods correct.
          const auto* original = dynamic_cast<const sun::ast::FunctionAST*>(
              ctx_.results()
                  .declarations.get(interfaceMethod.declarationId)
                  .astNode);
          if (original && original->hasBody() &&
              (!classDef.isPrecompiled() || interfaceMethod.isGeneric())) {
            auto copy = original->clone();
            auto function = std::unique_ptr<sun::ast::FunctionAST>(
                static_cast<sun::ast::FunctionAST*>(copy.release()));
            sun::ast::clearResolvedTypes(*function);
            function->setDeclarationId(method.declarationId);
            function->declarationIdentity().session =
                ctx_.results().declarations.session();
            sema_.pipeline().prepareGenerated(
                *function, classType->getDeclarationId(), original);
            ctx_.results().declarations.bindAstNode(method.declarationId,
                                                    function.get());
            auto& proto =
                const_cast<sun::ast::PrototypeAST&>(function->getProto());
            proto.setQualifiedName(
                classDef.getQualifiedName().memberNamed(method.name));
            proto.setResolvedParamTypes(method.paramTypes);
            proto.setResolvedReturnType(method.returnType);
            proto.setTypeBindings(original->getProto().getTypeBindings());
            function->setPrecompiled(false);
            const_cast<sun::ast::ClassDefinitionAST&>(classDef)
                .getMutableMethods()
                .push_back(
                    {std::move(function), false, interfaceMethod.isConst});
          }
          classType->bindInterfaceMethod(interfaceMethod.declarationId,
                                         method.declarationId);
          for (auto inherited : interfaceMethod.inheritedDeclarations)
            classType->bindInterfaceMethod(inherited, method.declarationId);
          method.visibility = interfaceMethod.visibility;
          method.isConst = interfaceMethod.isConst;
          method.isUnsafe = interfaceMethod.isUnsafe;
          method.canThrow = interfaceMethod.canThrow;

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
          methodInfo.canThrow = interfaceMethod.canThrow;
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

void InterfaceAnalyzer::prepareInterfaceShape(
    sun::ast::InterfaceDefinitionAST& interfaceDef) {
  const QualifiedName& qualifiedInterface = interfaceDef.getQualifiedName();
  std::string interfaceName = qualifiedInterface.lookupName();

  // Forbid redefinition of an interface in the same scope
  if (ctx_.declarations().isDeclared(interfaceDef.getName(), ctx_.scope())) {
    logAndThrowError(
        "Redefinition of interface '" + interfaceDef.getName() + "'",
        interfaceDef.getLocation());
  }

  // Validate interface name
  validateNotReserved(interfaceDef.getName(), "Interface name",
                      interfaceDef.getLocation());

  // Check for redefinition of builtin types
  if (ctx_.types()->isBuiltinTypeName(interfaceDef.getName())) {
    logAndThrowError(
        "Cannot redefine builtin interface '" + interfaceDef.getName() + "'",
        interfaceDef.getLocation());
  }

  // Validate field names and reject duplicates, on the same terms as classes:
  // a name-only comparison, ahead of the generic early-return below.
  validateFieldNames(interfaceDef.getFields(), "Interface field name",
                     "interface '" + interfaceDef.getName() + "'");

  // Validate method names
  for (const auto& methodDecl : interfaceDef.getMethods()) {
    const std::string& methodName = methodDecl.function->getProto().getName();
    validateNotReserved(methodName, "Interface method name",
                        methodDecl.function->getLocation());
  }

  // Lifetime declarations must be distinct, and every lifetime a member
  // names must be the builtin 'this or declared on the interface
  rejectDuplicateLifetimes(interfaceDef.getLifetimeParameters(),
                           " on interface '" + interfaceDef.getName() + "'");
  SemanticContext::LifetimeScopeGuard lifetimes(ctx_);
  for (const auto& lp : interfaceDef.getLifetimeParameters()) {
    ctx_.declareLifetime(lp.name);
  }
  {
    SemanticContext::LifetimeScopeGuard receiverLifetime(ctx_, true);
    if (interfaceDef.getParent())
      sema_.declarations().checkAnnotationLifetimes(*interfaceDef.getParent(),
                                                    interfaceDef.getLocation());
    for (const auto& field : interfaceDef.getFields()) {
      sema_.declarations().checkAnnotationLifetimes(field.type, field.location);
    }
    for (const auto& methodDecl : interfaceDef.getMethods()) {
      sema_.declarations().checkSignatureLifetimes(
          methodDecl.function->getProto(), methodDecl.function->getLocation());
    }
  }

  // Handle generic interfaces differently
  if (interfaceDef.isGeneric()) {
    // Register as generic interface template for later instantiation
    GenericInterfaceInfo info;
    info.AST = &interfaceDef;
    info.typeParameters = interfaceDef.getTypeParameters();
    info.qualifiedName = qualifiedInterface;
    ctx_.currentScope().declareGenericInterface(interfaceDef.getName(), info);

    // Create a generic interface type (for type checking generic
    // references)
    auto interfaceType = ctx_.types()->getGenericInterface(
        interfaceDef.getDeclarationId(), qualifiedInterface,
        interfaceDef.getTypeParameterNames());
    interfaceType->visibility = interfaceDef.getVisibility();
    interfaceType->setQualifiedName(qualifiedInterface);
    ctx_.currentScope().declareInterface(interfaceDef.getName(), interfaceType);

    if (interfaceDef.getParent()) {
      std::vector<TypePtr> arguments;
      for (size_t i = 0; i < interfaceDef.getTypeParameters().size(); ++i)
        arguments.push_back(interfaceDef.getTypeParameters()[i].toSunType(
            ctx_.results().declarations,
            interfaceDef.declarationIdentity().typeParameters.at(i)));
      generics_.instantiateGenericInterface(
          interfaceDef.getQualifiedName().lookupName(), arguments);
    }
    interfaceDef.setResolvedType(Types::Void());
    return;
  }

  // Non-generic interface: create the interface type directly
  auto interfaceType = ctx_.types()->getInterface(
      interfaceDef.getDeclarationId(), qualifiedInterface);
  interfaceType->setLifetimeParams(
      sun::ast::lifetimeParameterNames(interfaceDef.getLifetimeParameters()));
  // Store the user-written base name for error messages
  if (interfaceName != interfaceDef.getName()) {
    interfaceType->setBaseName(interfaceDef.getName());
  }
  interfaceType->visibility = interfaceDef.getVisibility();
  interfaceType->setQualifiedName(qualifiedInterface);

  // Add fields to the interface type and pseudo-class
  for (const auto& field : interfaceDef.getFields()) {
    TypePtr fieldType = resolver_.typeAnnotationToType(field.type);
    interfaceType->addField(field.name, fieldType, field.declaration.id)
        .visibility = field.visibility;
  }

  // Add methods to the interface type, rejecting duplicate signatures.
  MethodSignatureSet methodSignatures(ctx_, resolver_);
  for (const auto& methodDecl : interfaceDef.getMethods()) {
    // Get method signature info (pure computation)
    FunctionInfo methodInfo = sema_.declarations().getFunctionInfo(
        *methodDecl.function, !methodDecl.hasDefaultImpl);
    PrototypeAST& proto =
        const_cast<PrototypeAST&>(methodDecl.function->getProto());

    // Apply computed info to prototype
    sema_.declarations().applyFunctionInfoToProto(proto, methodInfo);

    if (!methodSignatures.insert(proto, methodInfo.paramTypes))
      logAndThrowError(
          "Interface method '" + proto.getName() + "' is already defined",
          methodDecl.function->getLocation());
    // Add method to interface type (include generic type parameters)
    auto& method = interfaceType->addMethod(
        proto.getName(), methodInfo.returnType, methodInfo.paramTypes,
        methodDecl.hasDefaultImpl, proto.getTypeParameterNames());
    method.declarationId = proto.getDeclarationId();
    for (size_t i = 0; i < proto.getTypeParameters().size(); ++i)
      method.genericArguments.push_back(proto.getTypeParameters()[i].toSunType(
          ctx_.results().declarations,
          proto.declarationIdentity().typeParameters.at(i)));
    {
      SemanticContext::ScopeSwitchGuard constraints(ctx_, ctx_.scope());
      ctx_.enterTypeParamScope(method.typeParameters, method.genericArguments);
      for (const auto& parameter : proto.getTypeParameters())
        method.genericConstraints.push_back(
            parameter.constraint
                ? resolver_.typeAnnotationToType(
                      parameter.constraint->toAnnotation(), true)
                : nullptr);
    }
    method.visibility =
        sun::semantic_analysis::methodVisibility(*methodDecl.function);
    method.isConst = methodDecl.isConst;
    method.isUnsafe = methodDecl.function->getProto().isUnsafeMethod();
    method.canThrow = proto.canThrow();
  }

  mergeInterfaceParent(*interfaceType, interfaceDef);
  ctx_.currentScope().declareInterface(interfaceDef.getName(), interfaceType);
  ctx_.declarations().noteDeclared(interfaceDef.getName(), ctx_.scope());
}

void InterfaceAnalyzer::analyzeInterfaceDefinition(
    sun::ast::InterfaceDefinitionAST& interfaceDef) {
  auto id = interfaceDef.getDeclarationId();
  ctx_.interfaceDefinitions.emplace(
      id, SemanticContext::InterfaceDefinition{&interfaceDef, ctx_.scope()});
  ensureInterfaceShape(id);
  if (!analyzedInterfaceBodies_.insert(id).second) return;
  interfaceDef.setResolvedType(Types::Void());
  if (interfaceDef.isGeneric()) return;
  auto interfaceType = ctx_.types()->getInterface(id);
  const auto& qualifiedInterface = interfaceDef.getQualifiedName();
  auto pseudoId = ctx_.results().declarations.add(
      sun::semantic_analysis::DeclarationKind::Class,
      "__interface_" + interfaceDef.getName(), id,
      ctx_.results().declarations.get(id).module, {}, id, "interface-receiver");
  auto pseudoClass = ctx_.types()->getClass(pseudoId);
  for (const auto& field : interfaceType->getFields())
    pseudoClass->addField(field.name, field.type, field.declarationId)
        .visibility = field.visibility;
  for (const auto& method : interfaceType->getMethods()) {
    auto& member =
        pseudoClass->addMethod(method.name, method.returnType,
                               method.paramTypes, false, method.typeParameters);
    member.declarationId = method.declarationId;
    member.visibility = method.visibility;
    member.isConst = method.isConst;
    member.isUnsafe = method.isUnsafe;
    member.canThrow = method.canThrow;
  }
  // Enter Interface scope to contain method scopes
  ctx_.enterInterfaceScope(qualifiedInterface);

  // Analyze default method bodies
  for (const auto& methodDecl : interfaceDef.getMethods()) {
    if (methodDecl.hasDefaultImpl) {
      // Set pseudo-class as ctx_.getCurrentClass() so 'this' works
      auto savedClass = ctx_.getCurrentClass();
      ctx_.setCurrentClass(pseudoClass);

      // Analyze the method body
      sema_.bodies().analyzeFunction(*methodDecl.function);

      // Restore original ctx_.getCurrentClass()
      ctx_.setCurrentClass(savedClass);
    }
  }

  ctx_.exitScope();  // Interface scope
}

TypePtr InterfaceAnalyzer::resolveInterfaceMemberType(
    const MemberAccessAST& memberAccess, const TypePtr& objectType,
    const std::string& memberName) {
  const auto* ifaceType =
      static_cast<const sun::types::InterfaceType*>(objectType.get());

  // Check for field
  const InterfaceField* field =
      ctx_.accessibleField(*ifaceType, memberName, memberAccess.getLocation());
  if (field) {
    memberAccess.setTargetDeclarationId(field->declarationId);
    return field->type;
  }

  // Check for method
  const InterfaceMethod* method =
      ctx_.accessibleMethod(*ifaceType, memberName, memberAccess.getLocation());
  if (method) {
    if (method->returnType && method->returnType->isInterface())
      logAndThrowError(
          "An interface return requirement can only be called on a concrete "
          "implementation",
          memberAccess.getLocation());
    memberAccess.setTargetDeclarationId(method->declarationId);
    return Types::Function(method->returnType, method->paramTypes,
                           method->canThrow, method->isUnsafe);
  }

  logAndThrowError("Unknown member '" + memberName + "' on interface '" +
                       ifaceType->toDisplayString() + "'",
                   memberAccess.getLocation());
}

}  // namespace sun::semantic_analysis
