// generic_specializer.cpp — Monomorphization (see generic_specializer.h)

#include "semantic_analysis/generic_specializer.h"

#include <algorithm>

#include "semantic_analysis/field_initialization.h"
#include "semantic_analysis/generic_type_arguments.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_rules.h"
#include "semantic_analysis/type_traits.h"
#include "support/error.h"

using sun::unwrapRef;
using sun::access::methodVisibility;
using sun::names::getFunctionSignature;
using sun::rules::isAssignableTo;

// -------------------------------------------------------------------
// Generic type parameter constraints
// -------------------------------------------------------------------

void GenericSpecializer::checkTypeParameterConstraints(
    const std::vector<TypeParameter>& typeParams,
    const std::vector<sun::TypePtr>& typeArgs, const std::string& what,
    const std::string& name, std::optional<Position> loc) {
  for (size_t i = 0; i < typeParams.size() && i < typeArgs.size(); ++i) {
    const auto& constraint = typeParams[i].constraint;
    if (!constraint) continue;

    // Inside a template body the argument is still a type parameter; the
    // real check happens when the enclosing generic is specialized.
    const sun::TypePtr& arg = typeArgs[i];
    if (!arg || arg->isTypeParameter()) continue;

    if (!sun::traits::satisfies(arg, constraint->qualifiedName
                                         ? constraint->qualifiedName->mangled()
                                         : constraint->name)) {
      // Point at the constraint itself when it carries a span; a declaration
      // parsed from a bundle has none, so fall back to the caller's location.
      std::optional<Position> at =
          constraint->span.endOffset.has_value()
              ? std::optional<Position>(constraint->span)
              : loc;
      logAndThrowError("type argument '" + arg->toDisplayString() +
                           "' does not satisfy constraint '" +
                           constraint->toString() + "' on type parameter '" +
                           typeParams[i].name + "' of " + what + " '" + name +
                           "'",
                       at);
    }
  }
}

// -------------------------------------------------------------------

const GenericClassInfo* GenericSpecializer::lookupGenericClassOf(
    const sun::ClassType& specialized) const {
  return ctx_.lookupGenericClass(specialized.getGenericQualifiedName());
}

// Scope a class's template was declared in: for a specialization, the
// generic's; for a plain class with generic methods, its own registration
// (hasGenericMethods() registers a GenericClassInfo too). nullptr if unknown.
SemanticScope* GenericSpecializer::classDefinitionScope(
    const sun::ClassType& classType) const {
  const GenericClassInfo* info =
      classType.isSpecialized()
          ? lookupGenericClassOf(classType)
          : ctx_.lookupGenericClass(classType.getQualifiedName());
  return info ? SemanticContext::definitionScopeOf(*info) : nullptr;
}

// Instantiate a generic class with specific type arguments
std::shared_ptr<sun::ClassType> GenericSpecializer::instantiateGenericClass(
    const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs) {
  auto* genericClassInfo = ctx_.lookupGenericClass(baseName);
  if (!genericClassInfo || !genericClassInfo->AST) {
    logAndThrowError("Unknown generic class '" + baseName + "'");
  }
  return instantiateGenericClass(*genericClassInfo, typeArgs);
}

std::shared_ptr<sun::ClassType> GenericSpecializer::instantiateGenericClass(
    const GenericClassInfo& info, const std::vector<sun::TypePtr>& typeArgs) {
  const GenericClassInfo* genericClassInfo = &info;
  const std::string& baseName = info.qualifiedName.baseName;

  // The template's members, interfaces and bodies are analyzed in the scope
  // the template was declared in, wherever this instantiation was requested
  // from: names resolve as they do at the definition site (transitive
  // dependencies of its module included) and access control sees the
  // template's own module.
  SemanticContext::ScopeSwitchGuard definitionScope(
      ctx_, SemanticContext::definitionScopeOf(*genericClassInfo));
  SemanticContext::SourceFileGuard definitionFile(
      ctx_, genericClassInfo->AST->getSourceFileId());

  // Verify type argument count matches
  if (typeArgs.size() != genericClassInfo->typeParameters.size()) {
    logAndThrowError("Generic class '" + baseName + "' expects " +
                     std::to_string(genericClassInfo->typeParameters.size()) +
                     " type arguments, got " + std::to_string(typeArgs.size()));
  }
  checkTypeParameterConstraints(genericClassInfo->typeParameters, typeArgs,
                                "generic class", baseName);

  // Construct the specialized QualifiedName from the generic's qualified name
  // with type arguments mangled into the base name
  sun::QualifiedName specializedQName;
  specializedQName.scopePath = genericClassInfo->qualifiedName.scopePath;
  specializedQName.modulePath = genericClassInfo->qualifiedName.modulePath;
  specializedQName.baseName = sun::Types::mangleGenericClassName(
      genericClassInfo->qualifiedName.baseName, typeArgs);

  // Derive the mangled name from the qualified name
  std::string mangledName = specializedQName.mangled();

  // `Unique<T>` where T is still a type parameter is not a specialization —
  // it is the template's own shape, named by a signature nobody has
  // instantiated yet (`create_unique<T>() Unique<T>`, or `ref Pair<T>` inside
  // `unwrap<T>`). The shape is still built, because a template body resolves
  // against it: `create_unique<T>`'s own body constructs `Unique<T>`, so it
  // needs those substituted members. What it must never become is something
  // to emit — codegen walks the specializations recorded on the generic and
  // would assert trying to lay out a type parameter. So it is never recorded
  // as one; the class the code actually uses is built when
  // `create_unique<i32>` is.
  bool abstractShape = false;
  for (const auto& arg : typeArgs) {
    if (arg && arg->isTypeParameter()) {
      abstractShape = true;
      break;
    }
  }

  // Check if already instantiated (both class type AND AST specialization).
  // An abstract shape records no AST, so having the type is all there is.
  auto existing = ctx_.lookupClass(specializedQName.baseName);
  if (existing && (abstractShape ||
                   genericClassInfo->AST->hasSpecialization(mangledName))) {
    // Both type and AST exist - nothing more to do
    return existing;
  }

  // Check if we're already in the process of instantiating this class
  // (breaks mutual recursion like Vec<T> <-> VecIterator<T>)
  if (classesBeingInstantiated_.count(mangledName)) {
    // Return the partially-created class type if it exists, or create a
    // placeholder This allows mutual references to be resolved
    if (existing) {
      return existing;
    }
    // Create and register a placeholder class type
    auto placeholder = ctx_.types()->getClass(specializedQName);
    ctx_.registerClass(specializedQName.baseName, placeholder);
    return placeholder;
  }

  // Track if we're only creating the AST (type already exists but AST doesn't)
  // This can happen when type is resolved (e.g., for a field type) before
  // a 'declare' statement explicitly instantiates it
  bool astOnlyMode = (existing != nullptr);

  // Mark this class as being instantiated to break mutual recursion
  classesBeingInstantiated_.insert(mangledName);

  // Create or reuse the specialized class type
  std::shared_ptr<sun::ClassType> specializedClass;
  if (astOnlyMode) {
    specializedClass = existing;
  } else {
    specializedClass = ctx_.types()->getClass(specializedQName);

    // Set type arguments for specialized class tracking
    // (getClass sets qualifiedName and baseName, but not type args)
    if (!specializedClass->isSpecialized()) {
      // This is a new class type - need to configure it as specialized
      // Get or create via getSpecializedClass for proper setup
      specializedClass = ctx_.types()->getSpecializedClass(
          genericClassInfo->qualifiedName.mangled(), typeArgs);
      specializedClass->setQualifiedName(specializedQName);
      specializedClass->setGenericQualifiedName(
          genericClassInfo->qualifiedName);
    }

    // The generic's declared lifetimes carry to every specialization -
    // the borrow checker entangles named arguments with receivers by them
    {
      std::vector<std::string> lifetimeNames;
      for (const auto& lp : genericClassInfo->AST->getLifetimeParameters()) {
        lifetimeNames.push_back(lp.name);
      }
      specializedClass->setLifetimeParams(std::move(lifetimeNames));
    }

    // Register the specialized class so methods can reference it
    ctx_.registerClass(specializedQName.baseName, specializedClass);

    // If this class was already fully instantiated in another scope
    // (fields populated + specialization AST created), just register in
    // current scope and return - avoids duplicating fields/methods/interfaces
    if (!specializedClass->getFields().empty() &&
        genericClassInfo->AST->hasSpecialization(mangledName)) {
      classesBeingInstantiated_.erase(mangledName);
      return specializedClass;
    }
  }

  // Push a scope for class-level type parameter bindings
  ctx_.enterClassScope(specializedQName);
  ctx_.addTypeParameterBindings(
      typeParameterNames(genericClassInfo->typeParameters), typeArgs);

  // Layout is a property of the generic definition, so every specialization
  // inherits it. Must precede the first getStructType(), which memoizes.
  specializedClass->setPacked(genericClassInfo->AST->isPacked());
  specializedClass->visibility = genericClassInfo->AST->getVisibility();

  // Add fields with substituted types (skip if type already exists or already
  // has fields from a previous instantiation in another scope)
  if (!astOnlyMode && specializedClass->getFields().empty()) {
    for (const auto& field : genericClassInfo->AST->getFields()) {
      auto fieldType = sema_.types().typeAnnotationToType(field.type);
      // Checked per specialization: whether a type argument is packable is
      // only knowable once T is substituted
      sema_.checkPackedFieldType(*genericClassInfo->AST, field, fieldType);
      specializedClass->addField(field.name, fieldType).visibility =
          field.visibility;
    }
  }

  // Handle implemented interfaces from the generic class definition
  // Substitute type parameters and add to the specialized class
  // Only do type processing when not in astOnlyMode to avoid infinite recursion
  // (e.g., Vec<T> implements IIterable<T, Vec<T>> - the Vec<T> arg would
  // trigger recursive instantiation)
  std::vector<ImplementedInterfaceAST> interfacesClone;
  for (const auto& ifaceRef :
       genericClassInfo->AST->getImplementedInterfaces()) {
    // Only process interface types if not in astOnlyMode
    if (!astOnlyMode) {
      std::shared_ptr<sun::InterfaceType> interfaceType;

      if (!ifaceRef.typeArguments.empty()) {
        // Generic interface with type arguments - substitute and instantiate
        std::vector<sun::TypePtr> ifaceTypeArgs;
        for (const auto& typeArg : ifaceRef.typeArguments) {
          ifaceTypeArgs.push_back(sema_.types().typeAnnotationToType(typeArg));
        }
        interfaceType =
            instantiateGenericInterface(ifaceRef.lookupName(), ifaceTypeArgs);
      } else {
        interfaceType = ctx_.lookupInterface(ifaceRef.lookupName());
      }

      // Add interface to class type
      if (interfaceType) {
        specializedClass->addImplementedInterface(interfaceType->getName());
      }
    }

    // Clone interface reference for specialized AST
    ImplementedInterfaceAST ifaceClone;
    ifaceClone.name = ifaceRef.name;
    ifaceClone.qualifiedName = ifaceRef.qualifiedName;
    for (const auto& ta : ifaceRef.typeArguments) {
      ifaceClone.typeArguments.push_back(ta);
    }
    interfacesClone.push_back(std::move(ifaceClone));
  }

  // Save old class context and set new one for method analysis
  auto savedClass = ctx_.getCurrentClass();
  ctx_.setCurrentClass(specializedClass);

  // Clone fields for specialized AST (TypeAnnotation as-is - codegen uses
  // ClassType for resolved types)
  std::vector<ClassFieldDecl> fieldsClone;
  for (const auto& field : genericClassInfo->AST->getFields()) {
    fieldsClone.push_back({field.name, field.type, field.location});
  }

  // Clone methods for specialized AST - each specialization gets its own
  // method ASTs so resolved types don't conflict between specializations
  std::vector<ClassMethodDecl> methodsClone;
  for (const auto& methodDecl : genericClassInfo->AST->getMethods()) {
    ClassMethodDecl methodClone;
    auto funcClone = methodDecl.function->clone();
    methodClone.function.reset(static_cast<FunctionAST*>(funcClone.release()));
    methodClone.isConstructor = methodDecl.isConstructor;
    methodClone.isConst = methodDecl.isConst;
    methodsClone.push_back(std::move(methodClone));
  }

  // PASS 1: Register all methods first (so methods can call each other)
  // In astOnlyMode, we skip type-system registration but still resolve types
  // for the cloned method prototypes
  for (size_t i = 0; i < methodsClone.size(); ++i) {
    const auto& methodClone = methodsClone[i];
    const auto& proto = methodClone.function->getProto();

    // Resolve class bindings while retaining declared method parameters.
    auto signature = sema_.getFunctionInfo(*methodClone.function);
    sun::TypePtr returnType = signature.returnType;
    std::vector<sun::TypePtr> paramTypes = std::move(signature.paramTypes);

    // Add method to class type (skip if type already exists)
    if (!astOnlyMode) {
      auto& method = specializedClass->addMethod(
          proto.getName(), returnType, paramTypes, methodClone.isConstructor,
          proto.getTypeParameterNames(), proto.canThrow());
      method.visibility = methodVisibility(*methodClone.function);
      method.isConst = methodClone.isConst;
    }

    // Update the cloned method's prototype with resolved types
    PrototypeAST& clonedProto =
        const_cast<PrototypeAST&>(methodClone.function->getProto());
    clonedProto.setResolvedParamTypes(paramTypes);
    clonedProto.setResolvedReturnType(returnType);

    // Store class-level type bindings on the prototype so codegen can resolve
    // type parameters (e.g., T -> f32 for Vec<f32> methods that use _store<T>)
    std::vector<std::pair<std::string, sun::TypePtr>> bindings;
    for (size_t i = 0;
         i < genericClassInfo->typeParameters.size() && i < typeArgs.size();
         ++i) {
      bindings.emplace_back(genericClassInfo->typeParameters[i].name,
                            typeArgs[i]);
    }
    clonedProto.setTypeBindings(std::move(bindings));

    // Register the method as a function with mangled name (skip if type already
    // exists)
    if (!astOnlyMode) {
      std::string methodMangledName =
          specializedClass->getMangledMethodName(proto.getName());

      // For methods, add 'this' as first parameter type
      std::vector<sun::TypePtr> methodParamTypes;
      methodParamTypes.push_back(specializedClass);  // this parameter
      for (const auto& pt : paramTypes) {
        methodParamTypes.push_back(pt);
      }
      ctx_.registerFunctionInCurrentScope(methodMangledName,
                                          {returnType, methodParamTypes, {}});
    }
  }

  // For precompiled non-generic classes, skip PASS 2 - method bodies are
  // already compiled in the linked bitcode BUT for generic classes (even
  // precompiled), we need PASS 2 because method bodies contain T that needs
  // substitution
  bool needsPass2 = !genericClassInfo->typeParameters.empty();
  if (genericClassInfo->AST->isPrecompiled() && !needsPass2) {
    // Create specialized AST even for precompiled classes
    auto specializedAST = std::make_shared<ClassDefinitionAST>(
        mangledName, std::vector<TypeParameter>{}, std::move(interfacesClone),
        std::move(fieldsClone), std::move(methodsClone),
        genericClassInfo->AST->isPrecompiled());
    specializedAST->setLifetimeParameters(
        genericClassInfo->AST->getLifetimeParameters());
    specializedAST->setIsPacked(genericClassInfo->AST->isPacked());
    specializedAST->setVisibility(genericClassInfo->AST->getVisibility());

    // Store specialization on the generic class AST for codegen access
    if (!abstractShape) {
      genericClassInfo->AST->addSpecialization(mangledName, specializedAST);
    }

    // Restore old class context
    ctx_.setCurrentClass(savedClass);
    ctx_.exitScope();
    // Remove from "being instantiated" set now that we're done
    classesBeingInstantiated_.erase(mangledName);
    return specializedClass;
  }

  // Create the specialized ClassDefinitionAST with the cloned methods (bodies
  // analyzed below, through this AST).
  // Note: Specializations are NOT precompiled - even if the generic class
  // came from a precompiled .moon file, new specializations with user-defined
  // type args (e.g., Unique<Point>) need codegen since they don't exist in
  // the library bitcode.
  auto specializedAST = std::make_shared<ClassDefinitionAST>(
      mangledName,                   // e.g., "Vec_i32" instead of "Vec"
      std::vector<TypeParameter>{},  // empty - no longer generic
      std::move(interfacesClone), std::move(fieldsClone),
      std::move(methodsClone),  // cloned methods
      false);                   // NOT precompiled - needs codegen
  // Lifetime declarations are erased from specialization keys but stay on
  // the specialized definition: the borrow checker reads them per class
  specializedAST->setLifetimeParameters(
      genericClassInfo->AST->getLifetimeParameters());
  specializedAST->setIsPacked(genericClassInfo->AST->isPacked());
  specializedAST->setVisibility(genericClassInfo->AST->getVisibility());
  specializedAST->setLocation(genericClassInfo->AST->getLocation());
  specializedAST->inheritSourceFile(genericClassInfo->AST->getSourceFileId());

  // Interface conformance is checked per specialization (signatures are
  // only known once T is substituted)
  if (!astOnlyMode) {
    sema_.validateInterfaceImplementation(*specializedAST, specializedClass);
  }

  // Store specialization on the generic class AST for codegen access
  if (!abstractShape) {
    genericClassInfo->AST->addSpecialization(mangledName, specializedAST);
  }

  // An abstract shape's bodies are never checked here. Only its members are
  // wanted, and those are in place by now; the bodies are checked when a real
  // specialization is made, which is also the only version anyone emits.
  // Checking them here would mean checking them against a type argument that
  // is not a type — `Thread<_return_type_of<F>>` names what F returns, which
  // nothing can answer until F is known.
  if (abstractShape) {
    ctx_.setCurrentClass(savedClass);
    ctx_.exitScope();
    classesBeingInstantiated_.erase(mangledName);
    return specializedClass;
  }

  // PASS 2: Analyze all cloned method bodies — unless requested from the
  // declaration pre-pass, where bodies are deferred until every declaration
  // (including functions the bodies may call) is registered.
  bool deferBodies = inPrepass_;
  if (!deferBodies) {
    for (auto& methodClone : specializedAST->getMutableMethods()) {
      FunctionAST* methodFunc = methodClone.function.get();
      const auto& proto = methodFunc->getProto();

      // Skip generic methods - they are analyzed when called with type args
      if (proto.isGeneric()) {
        continue;
      }

      // Use the unified helper (type params already in outer scope, pass
      // empty). This analyzes the CLONED method, not the shared generic one
      sema_.analyzeMethodWithBindings(*methodFunc, specializedClass, {}, {});
    }
    // Each specialization's constructors are checked against its own fields:
    // what a field's type turns out to be is only known here
    for (const auto& methodClone : specializedAST->getMethods()) {
      if (!methodClone.isConstructor) continue;
      if (methodClone.function->getProto().isGeneric()) continue;
      checkFieldInitialization(*methodClone.function, *specializedClass,
                               specializedAST->getMethods());
    }
  }

  if (deferBodies) {
    deferredSpecializations_.push_back(
        {specializedClass, genericClassInfo, typeArgs, specializedAST});
  }

  // Restore old class context
  ctx_.setCurrentClass(savedClass);

  // Pop the class-level type parameter scope
  ctx_.exitScope();

  // Remove from "being instantiated" set now that we're done
  classesBeingInstantiated_.erase(mangledName);

  return specializedClass;
}

// Analyze the method bodies of specializations created during the
// declaration pre-pass. Re-enters an equivalent class scope (type parameter
// bindings) inside the template's definition scope. Bodies may create
// further specializations; those are analyzed immediately (the pre-pass is
// over) so the loop is by index.
void GenericSpecializer::analyzeDeferredSpecializations() {
  for (size_t i = 0; i < deferredSpecializations_.size(); ++i) {
    DeferredSpecialization d = deferredSpecializations_[i];
    SemanticContext::ScopeSwitchGuard definitionScope(
        ctx_, SemanticContext::definitionScopeOf(*d.genericInfo));
    SemanticContext::SourceFileGuard definitionFile(
        ctx_, d.genericInfo->AST->getSourceFileId());

    ctx_.enterClassScope(d.specializedClass->getQualifiedName());
    ctx_.addTypeParameterBindings(
        typeParameterNames(d.genericInfo->typeParameters), d.typeArgs);
    auto savedClass = ctx_.getCurrentClass();
    ctx_.setCurrentClass(d.specializedClass);

    for (auto& methodClone : d.specializedAST->getMutableMethods()) {
      FunctionAST* methodFunc = methodClone.function.get();
      if (methodFunc->getProto().isGeneric()) continue;
      sema_.analyzeMethodWithBindings(*methodFunc, d.specializedClass, {}, {});
    }
    for (const auto& methodClone : d.specializedAST->getMethods()) {
      if (!methodClone.isConstructor) continue;
      if (methodClone.function->getProto().isGeneric()) continue;
      checkFieldInitialization(*methodClone.function, *d.specializedClass,
                               d.specializedAST->getMethods());
    }

    ctx_.setCurrentClass(savedClass);
    ctx_.exitScope();
  }
  deferredSpecializations_.clear();
}

// -------------------------------------------------------------------
// Value packs
// -------------------------------------------------------------------

std::optional<std::vector<sun::TypePtr>> GenericSpecializer::splitPackArgTypes(
    const PrototypeAST& proto, const std::vector<sun::TypePtr>& argTypes,
    const std::string& displayName, std::optional<Position> loc) {
  if (!proto.hasVariadicParam()) return std::nullopt;

  const size_t fixed = proto.getArgs().size();
  if (argTypes.size() < fixed) {
    logAndThrowError("'" + displayName + "' expects at least " +
                         std::to_string(fixed) +
                         (fixed == 1 ? " argument, got " : " arguments, got ") +
                         std::to_string(argTypes.size()),
                     loc);
  }
  return std::vector<sun::TypePtr>(argTypes.begin() + fixed, argTypes.end());
}

void GenericSpecializer::declareVariadicPack(const PrototypeAST& proto) {
  if (!proto.hasVariadicParam()) return;
  auto* fnScope = ctx_.currentFunctionScope();
  if (!fnScope) return;

  const VariadicParam& pack = proto.getVariadicParam();
  const auto& types = proto.getResolvedVariadicTypes();

  // The pack itself, so `args...` in the body can be expanded into concrete
  // typed arguments. Recorded even when empty — an empty pack expands to no
  // arguments, which is different from having no pack. ctx_.exitScope() drops
  // it.
  fnScope->variadicParam = {pack.name, types};

  // Its elements, under the names codegen gives the parameters. The expansion
  // rewrites `args...` into references to exactly these.
  for (size_t i = 0; i < types.size(); ++i) {
    ctx_.declareVariable(pack.elementName(i), types[i], /*isParam=*/true);
  }
}

void GenericSpecializer::applyVariadicParamTypes(
    PrototypeAST& clonedProto, const PrototypeAST& proto,
    const std::vector<sun::TypePtr>& variadicArgTypes,
    std::optional<Position> loc) {
  // The pack's arity and element types come from the call, which is what lets
  // one `create<Point>` site select init(i32) and another init(i32, i32).
  clonedProto.setResolvedVariadicTypes(variadicArgTypes);

  // A bare `args...` takes whatever the call passes. Anything other than
  // `_params_of<T>` is recorded and left unchecked.
  if (!proto.hasVariadicTypeAnnotation()) return;
  const TypeAnnotation& annot = proto.getVariadicTypeAnnotation();
  if (annot.baseName != "_params_of" || annot.typeArguments.empty()) return;

  sun::TypePtr target =
      sema_.types().typeAnnotationToType(*annot.typeArguments[0]);
  if (!target) return;

  const std::string got = "(" + sun::formatTypeList(variadicArgTypes) + ")";

  // `_params_of<C>` for a class: the parameters of C's constructor. Which
  // overload is selected happens downstream at _init, via lookupConstructor;
  // here we only check that one of them matches.
  if (target->isClass()) {
    auto* targetClass = static_cast<sun::ClassType*>(target.get());
    if (targetClass->getMethod("init") &&
        !targetClass->getMethodForArgs("init", variadicArgTypes)) {
      logAndThrowError("No matching constructor for '" +
                           target->toDisplayString() + "' with arguments " +
                           got,
                       loc);
    }
    return;
  }

  // `_params_of<F>` for a lambda or named-function value: the parameters it
  // takes. There is only one parameter list, so a mismatch is an error
  // rather than a failed overload match.
  const std::vector<sun::TypePtr>* params = nullptr;
  if (target->isLambda()) {
    params = &static_cast<sun::LambdaType*>(target.get())->getParamTypes();
  } else if (target->isFunction()) {
    params = &static_cast<sun::FunctionType*>(target.get())->getParamTypes();
  }
  if (params) {
    bool matches = params->size() == variadicArgTypes.size();
    for (size_t i = 0; matches && i < params->size(); ++i) {
      matches = isAssignableTo(variadicArgTypes[i], (*params)[i]);
    }
    if (!matches) {
      logAndThrowError("Pack '" + proto.getVariadicParamName() +
                           "' fills the parameters of " +
                           target->toDisplayString() + ", which takes (" +
                           sun::formatTypeList(*params) + "); got " + got,
                       loc);
    }
  }
}

// -------------------------------------------------------------------
// Generic function support
// -------------------------------------------------------------------

// Type-argument inference for calls lives in generic_type_arguments.cpp.

SpecializedFunctionInfo GenericSpecializer::requireGenericSpecialization(
    const GenericFunctionInfo& genericInfo,
    const std::vector<sun::TypePtr>& typeArgs, const std::string& displayName,
    std::optional<Position> loc,
    const std::optional<std::vector<sun::TypePtr>>& variadicArgTypes) {
  auto specialized =
      instantiateGenericFunction(genericInfo, typeArgs, variadicArgTypes);
  if (!specialized) {
    logAndThrowError("Failed to instantiate generic function '" + displayName +
                         "' with the given type arguments",
                     loc);
  }
  return *specialized;
}

std::optional<SpecializedFunctionInfo>
GenericSpecializer::instantiateGenericFunction(
    const GenericFunctionInfo& genericInfo,
    const std::vector<sun::TypePtr>& typeArgs,
    const std::optional<std::vector<sun::TypePtr>>& variadicArgTypes) {
  const FunctionAST* genericFunc = genericInfo.AST;
  if (!genericFunc) {
    return std::nullopt;
  }
  const PrototypeAST& proto = genericFunc->getProto();

  // A pack's arity and types come from the actual call arguments. Without
  // them (nullopt, e.g. from type inference) there is nothing to specialize
  // yet; the call-site trigger supplies them and does the real work.
  if (proto.hasVariadicParam() && !variadicArgTypes) {
    return std::nullopt;
  }

  // Inside a template body the arguments are still type parameters; the real
  // specialization is made when the enclosing generic gets concrete types.
  auto isTypeParam = [](const sun::TypePtr& t) {
    return t && t->isTypeParameter();
  };
  if (variadicArgTypes && std::any_of(variadicArgTypes->begin(),
                                      variadicArgTypes->end(), isTypeParam)) {
    return std::nullopt;
  }

  // Name the specialization off the template's registration, which includes
  // enclosing function context (e.g. outer_i32_inner) and is fixed from the
  // moment the template is collected. The prototype's own mangled name only
  // gains its overload suffix once its definition is analyzed, so using it
  // would name the same specialization differently depending on whether the
  // call site sits above or below the definition.
  std::vector<std::string> typeParams = proto.getTypeParameterNames();
  // How the template is named in diagnostics below — as it was written, not
  // as it is emitted, so a stdlib template arriving through a bundle reads as
  // `std.thread.spawn` rather than `$ce09fa07$_std_thread_spawn`.
  const std::string funcName = genericInfo.qualifiedName.empty()
                                   ? proto.getMangledName()
                                   : genericInfo.qualifiedName.display();

  // The name the specialization is emitted under: the template's scope and
  // module, with the type arguments folded into the base name. Codegen calls
  // the name semantic analysis records on the call.
  const sun::QualifiedName specializedName =
      sun::QualifiedName::specializationOf(
          genericInfo.qualifiedName, typeArgs,
          variadicArgTypes.value_or(std::vector<sun::TypePtr>{}));
  // Symbol form, used as the key of every specialization table below.
  std::string mangledName = specializedName.mangled();

  // Check cache first
  auto cacheIt = specializedFunctionCache_.find(mangledName);
  if (cacheIt != specializedFunctionCache_.end()) {
    return cacheIt->second;
  }

  // Also check if specialization exists on the generic function AST
  if (genericFunc->hasSpecialization(mangledName)) {
    // Rebuild the info from the stored AST (captures/types may need recompute)
    // This shouldn't happen often since cache is checked first
  }

  // Verify type argument count
  if (typeArgs.size() != typeParams.size()) {
    logAndThrowError("Generic function '" + funcName + "' expects " +
                     std::to_string(typeParams.size()) +
                     " type arguments, got " + std::to_string(typeArgs.size()));
  }
  checkTypeParameterConstraints(proto.getTypeParameters(), typeArgs,
                                "generic function", funcName,
                                proto.getLocation());

  // Enter scope and bind type parameters
  // Analyze the body in the scope the template was declared in (a module,
  // or the enclosing specialized function for nested generics — that is
  // where its captures and outer type bindings live)
  SemanticContext::ScopeSwitchGuard definitionScope(
      ctx_, SemanticContext::definitionScopeOf(genericInfo));
  SemanticContext::SourceFileGuard definitionFile(
      ctx_, genericInfo.AST->getSourceFileId());
  ctx_.enterTypeParamScope(typeParams, typeArgs);

  // Substitute parameter types
  std::vector<sun::TypePtr> paramTypes;
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = sema_.types().typeAnnotationToType(argType);

    // If a type parameter resolved to a compound type, error - must use ref
    if (paramType && sun::typeMovesOnRead(paramType)) {
      logAndThrowError("Parameter '" + argName + "' has compound type '" +
                           paramType->toDisplayString() +
                           "' which cannot be passed by value. Use 'ref " +
                           paramType->toDisplayString() + "' instead.",
                       genericFunc->getLocation());
    }

    paramTypes.push_back(paramType);
  }

  // Substitute return type
  sun::TypePtr returnType;
  if (proto.hasReturnType()) {
    returnType = sema_.types().typeAnnotationToType(*proto.getReturnType());
  } else {
    returnType = sun::Types::Void();
  }

  // Substitute capture types (field-by-field rebuild; keep byRef intact)
  std::vector<Capture> substitutedCaptures;
  for (const auto& cap : proto.getCaptures()) {
    Capture subCap = cap;
    subCap.type = sema_.types().substituteTypeParameters(cap.type);
    substitutedCaptures.push_back(subCap);
  }

  // Clone the function AST and re-analyze with type parameter bindings
  std::shared_ptr<FunctionAST> specializedAST = nullptr;
  if (genericFunc->hasBody()) {
    // Clone the entire function AST
    auto cloned = genericFunc->clone();
    auto clonedFunc = std::unique_ptr<FunctionAST>(
        static_cast<FunctionAST*>(cloned.release()));

    // Update the prototype for the specialized function:
    // - Set the mangled name (e.g., "foo_i32" instead of "foo")
    // - Clear type parameters so it's no longer treated as generic
    // - Set resolved types so codegen can use them directly
    // - Store type bindings for nested generic call resolution
    PrototypeAST& clonedProto =
        const_cast<PrototypeAST&>(clonedFunc->getProto());
    // The qualified name is the specialization's identity: it is the symbol
    // codegen emits and calls, and it qualifies nested functions declared in
    // this body (e.g. outer_i32 rather than outer). The prototype's plain name
    // stays the one written in the source.
    clonedProto.setQualifiedName(specializedName);

    // Build and store type parameter bindings (e.g., T -> i32)
    // These are used by codegen to resolve nested generic calls
    std::vector<std::pair<std::string, sun::TypePtr>> bindings;
    for (size_t i = 0; i < typeParams.size() && i < typeArgs.size(); ++i) {
      bindings.emplace_back(typeParams[i], typeArgs[i]);
    }
    clonedProto.setTypeBindings(std::move(bindings));

    clonedProto.clearTypeParameters();
    clonedProto.setResolvedParamTypes(paramTypes);
    clonedProto.setResolvedReturnType(returnType);
    clonedProto.setCaptures(substitutedCaptures);

    if (variadicArgTypes) {
      applyVariadicParamTypes(clonedProto, proto, *variadicArgTypes,
                              genericFunc->getLocation());
    }

    // Clear resolved types for fresh analysis
    sema_.clearResolvedTypes(*clonedFunc);

    // Compute function signature for nested function qualification
    std::string funcSig = getFunctionSignature(mangledName, paramTypes);

    // Declare parameters in scope for body analysis - use the mangled qualified
    // name so nested functions get correct context
    ctx_.enterFunctionScope(funcSig, clonedProto.getQualifiedName(),
                            proto.canThrow(),
                            clonedProto.getResolvedReturnType());

    declareVariadicPack(clonedProto);

    for (size_t i = 0; i < paramTypes.size(); ++i) {
      // Use parameter names from the cloned prototype
      std::string argName = proto.getArgs()[i].first;
      ctx_.declareVariable(argName, paramTypes[i], /*isParam=*/true);
    }

    // Add captures to scope
    for (const auto& cap : substitutedCaptures) {
      ctx_.declareVariable(cap.name, cap.type);
    }

    // Analyze the body with current type parameter bindings
    sema_.analyzeBlock(const_cast<BlockExprAST&>(clonedFunc->getBody()));

    ctx_.exitScope();  // parameter scope

    specializedAST = std::move(clonedFunc);
    // Specializations are NOT precompiled - even if the generic function
    // came from a precompiled .moon file, new specializations need codegen
    // since they don't exist in the library bitcode.
    specializedAST->setPrecompiled(false);
  }

  ctx_.exitScope();  // type parameter scope

  // Build result. A pack's elements are ordinary positional parameters after
  // the fixed ones, so the call's signature is the two lists joined — that is
  // the order codegen emits them in, and what every argument check downstream
  // lines up against.
  SpecializedFunctionInfo result;
  result.qualifiedName = specializedName;
  result.returnType = returnType;
  if (variadicArgTypes) {
    paramTypes.insert(paramTypes.end(), variadicArgTypes->begin(),
                      variadicArgTypes->end());
  }
  result.paramTypes = std::move(paramTypes);
  result.captures = std::move(substitutedCaptures);
  result.specializedAST = specializedAST;

  // Store specialization on the generic function AST for codegen access
  if (specializedAST) {
    genericFunc->addSpecialization(mangledName, specializedAST);
  }

  // Cache and return
  specializedFunctionCache_[mangledName] = result;
  return result;
}

// -------------------------------------------------------------------
// Generic method instantiation
// -------------------------------------------------------------------

// Find the generic method's FunctionAST on a class by name. Resolves the class
// definition (specialized AST when available, else the generic definition) and
// returns the first generic method matching `methodName`, or nullptr.
FunctionAST* GenericSpecializer::findGenericMethodAST(
    const sun::ClassType* classType, const std::string& methodName) {
  if (!classType) return nullptr;

  const ClassDefinitionAST* classDef = nullptr;
  // For specialized classes, look up the method from the SPECIALIZED class AST
  // (not the base generic class), because that's what codegen will iterate over
  if (classType->isSpecialized()) {
    auto* genericInfo = lookupGenericClassOf(*classType);
    if (genericInfo && genericInfo->AST) {
      auto specAST =
          genericInfo->AST->getSpecialization(classType->getMangledName());
      classDef = specAST ? specAST.get() : genericInfo->AST;
    }
  } else if (classType->isGenericDefinition()) {
    auto* genericInfo = ctx_.lookupGenericClass(classType->getBaseName());
    if (genericInfo) classDef = genericInfo->AST;
  } else {
    // Non-generic class - may still have generic methods
    auto* genericInfo = ctx_.lookupGenericClass(classType->getBaseName());
    if (genericInfo) classDef = genericInfo->AST;
  }

  if (!classDef) return nullptr;

  // End of instantiateGenericMethod

  for (const auto& methodDecl : classDef->getMethods()) {
    if (methodDecl.function->getProto().getName() == methodName &&
        methodDecl.function->getProto().isGeneric()) {
      return methodDecl.function.get();
    }
  }
  return nullptr;
}

std::shared_ptr<FunctionAST> GenericSpecializer::instantiateGenericMethod(
    std::shared_ptr<sun::ClassType> classType, const std::string& methodName,
    const std::vector<sun::TypePtr>& methodTypeArgs,
    const std::optional<std::vector<sun::TypePtr>>& variadicArgTypes) {
  if (!classType || methodName.empty() || methodTypeArgs.empty()) {
    return nullptr;
  }

  // Inside a generic template body (analyzed with T bound to itself), the type
  // args are still type parameters; a real specialization is created when the
  // enclosing generic is instantiated with concrete types.
  auto isTypeParam = [](const sun::TypePtr& t) {
    return t && t->isTypeParameter();
  };
  if (std::any_of(methodTypeArgs.begin(), methodTypeArgs.end(), isTypeParam) ||
      (variadicArgTypes && std::any_of(variadicArgTypes->begin(),
                                       variadicArgTypes->end(), isTypeParam))) {
    return nullptr;
  }

  // Build the specialized mangled name off "ClassName_methodName". The pack
  // suffix keys the specialization on the actual variadic argument types, so
  // overloaded factories (e.g. create<Point>(7) vs create<Point>(3,4)) get
  // distinct specializations. Codegen rebuilds this identically.
  const sun::QualifiedName specializedName =
      sun::QualifiedName::specializationOf(
          classType->getQualifiedName().memberNamed(methodName), methodTypeArgs,
          variadicArgTypes.value_or(std::vector<sun::TypePtr>{}));
  std::string mangledName = specializedName.mangled();

  // Check cache
  auto cacheIt = specializedFunctionCache_.find(mangledName);
  if (cacheIt != specializedFunctionCache_.end() &&
      cacheIt->second.specializedAST) {
    return cacheIt->second.specializedAST;
  }

  // Find the method's FunctionAST from the class definition
  FunctionAST* genericMethodAST =
      findGenericMethodAST(classType.get(), methodName);
  if (!genericMethodAST) {
    return nullptr;  // Generic method not found
  }

  const PrototypeAST& proto = genericMethodAST->getProto();
  const auto& methodTypeParams = proto.getTypeParameters();

  // A variadic method's arity/types come from the actual call arguments. If we
  // weren't given them (nullopt, e.g. invoked from type inference), defer: the
  // call-site trigger will specialize with the real variadic arg types.
  if (proto.hasVariadicParam() && !variadicArgTypes) {
    return nullptr;
  }

  if (methodTypeArgs.size() != methodTypeParams.size()) {
    logAndThrowError(
        "Type argument count mismatch for generic method " + methodName,
        genericMethodAST->getLocation());
    return nullptr;
  }
  checkTypeParameterConstraints(methodTypeParams, methodTypeArgs,
                                "generic method", methodName,
                                genericMethodAST->getLocation());

  // Set up scopes for type substitution:
  // 1. Class-level type parameters (if specialized generic class)
  // 2. Method-level type parameters

  std::vector<std::string> allTypeParams;
  std::vector<sun::TypePtr> allTypeArgs;

  // Collect class-level type parameter bindings for specialized generic classes
  if (classType->isSpecialized()) {
    auto* genericInfo = lookupGenericClassOf(*classType);
    if (genericInfo) {
      std::vector<std::string> classTypeParams =
          typeParameterNames(genericInfo->typeParameters);
      const auto& classTypeArgs = classType->getTypeArguments();
      for (size_t i = 0; i < classTypeParams.size() && i < classTypeArgs.size();
           ++i) {
        allTypeParams.push_back(classTypeParams[i]);
        allTypeArgs.push_back(classTypeArgs[i]);
      }
    }
  }

  // Add method-level type parameter bindings
  for (size_t i = 0; i < methodTypeParams.size(); ++i) {
    allTypeParams.push_back(methodTypeParams[i].name);
    allTypeArgs.push_back(methodTypeArgs[i]);
  }

  // Enter scope and add all type bindings, inside the class's definition
  // scope so the body resolves names as written at the definition site
  SemanticContext::ScopeSwitchGuard definitionScope(
      ctx_, classDefinitionScope(*classType));
  SemanticContext::SourceFileGuard definitionFile(
      ctx_, genericMethodAST->getSourceFileId());
  ctx_.enterTypeParamScope(allTypeParams, allTypeArgs);

  // Substitute types in parameters
  std::vector<sun::TypePtr> paramTypes;
  for (const auto& [argName, argType] : proto.getArgs()) {
    paramTypes.push_back(sema_.types().typeAnnotationToType(argType));
  }

  // Substitute return type
  sun::TypePtr returnType =
      proto.hasReturnType()
          ? sema_.types().typeAnnotationToType(*proto.getReturnType())
          : sun::Types::Void();

  // Clone the function AST for specialization
  // clone() returns unique_ptr<ExprAST>, so cast to FunctionAST
  auto cloned = genericMethodAST->clone();
  auto clonedFunc =
      std::unique_ptr<FunctionAST>(static_cast<FunctionAST*>(cloned.release()));

  PrototypeAST& clonedProto = const_cast<PrototypeAST&>(clonedFunc->getProto());

  // Name the specialization here, where it is made; call sites copy this name
  // rather than spelling one of their own.
  clonedProto.setQualifiedName(specializedName);

  // Store type bindings on the prototype
  std::vector<std::pair<std::string, sun::TypePtr>> bindings;
  for (size_t i = 0; i < allTypeParams.size(); ++i) {
    bindings.emplace_back(allTypeParams[i], allTypeArgs[i]);
  }
  clonedProto.setTypeBindings(std::move(bindings));

  // Clear type parameters (this is no longer a generic method)
  clonedProto.clearTypeParameters();

  // Store resolved types on prototype for codegen
  clonedProto.setResolvedParamTypes(paramTypes);
  clonedProto.setResolvedReturnType(returnType);

  if (proto.hasVariadicParam() && variadicArgTypes) {
    applyVariadicParamTypes(clonedProto, proto, *variadicArgTypes,
                            genericMethodAST->getLocation());
  }

  // Clear any stale resolved types from previous specializations
  sema_.clearResolvedTypes(*clonedFunc);

  // Analyze the method body
  auto savedClass = ctx_.getCurrentClass();
  ctx_.setCurrentClass(classType);

  // Compute method signature for nested function qualification
  std::string methodSig = getFunctionSignature(mangledName, paramTypes);

  // Enter method scope and declare parameters. A const method body sees the
  // const view of its return type (borrows of `this` are `const ref` there).
  sun::TypePtr bodyReturnType = proto.getResolvedReturnType();
  if (proto.isConstMethod())
    bodyReturnType = sema_.types().createConstView(bodyReturnType);
  ctx_.enterFunctionScope(
      methodSig,
      sun::QualifiedName(classType->getQualifiedName().scopePath, mangledName),
      proto.canThrow(), bodyReturnType);

  declareVariadicPack(clonedProto);

  // Declare 'this' parameter (immutable inside a const method)
  ctx_.declareVariable("this", classType, /*isParam=*/true,
                       /*isConst=*/clonedProto.isConstMethod());

  // Declare regular parameters
  for (size_t i = 0; i < paramTypes.size(); ++i) {
    const auto& [argName, argType] = proto.getArgs()[i];
    ctx_.declareVariable(argName, paramTypes[i], /*isParam=*/true);
  }

  // Analyze the body
  sema_.analyzeBlock(const_cast<BlockExprAST&>(clonedFunc->getBody()));

  ctx_.exitScope();  // method scope
  ctx_.setCurrentClass(savedClass);
  ctx_.exitScope();  // type parameter scope

  // Convert to shared_ptr for storage
  std::shared_ptr<FunctionAST> specializedAST = std::move(clonedFunc);
  // Specializations are NOT precompiled - they need codegen
  specializedAST->setPrecompiled(false);

  // Store specialization on the generic method AST for codegen access
  genericMethodAST->addSpecialization(mangledName, specializedAST);

  // Cache the result
  SpecializedFunctionInfo result;
  result.returnType = returnType;
  result.paramTypes = paramTypes;
  result.specializedAST = specializedAST;
  specializedFunctionCache_[mangledName] = result;

  return specializedAST;
}

using sun::access::methodVisibility;

// -------------------------------------------------------------------
// Interface support
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Generic interface support
// -------------------------------------------------------------------

std::shared_ptr<sun::InterfaceType>
GenericSpecializer::instantiateGenericInterface(
    const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs) {
  // Look up the generic interface definition first
  auto* genericInfo = ctx_.lookupGenericInterface(baseName);

  // Use the AST's mangled name for generating specialized interface name
  std::string effectiveBase = (genericInfo && genericInfo->AST)
                                  ? genericInfo->qualifiedName.mangled()
                                  : baseName;

  // Generate mangled name for the specialized interface
  std::string mangledName =
      sun::Types::mangleGenericClassName(effectiveBase, typeArgs);

  // Check if already instantiated
  auto existing = ctx_.lookupInterface(mangledName);
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
  checkTypeParameterConstraints(genericInfo->typeParameters, typeArgs,
                                "generic interface", baseName);

  // Create the specialized interface type
  auto specializedInterface =
      ctx_.types()->getSpecializedInterface(effectiveBase, typeArgs);
  specializedInterface->setGenericQualifiedName(genericInfo->qualifiedName);
  specializedInterface->visibility = genericInfo->AST->getVisibility();
  specializedInterface->setQualifiedName(
      sun::QualifiedName(genericInfo->qualifiedName.scopePath,
                         sun::Types::mangleGenericClassName(
                             genericInfo->qualifiedName.baseName, typeArgs),
                         genericInfo->qualifiedName.modulePath));

  {
    // Member annotations resolve in the interface's definition scope; the
    // result is registered in the requesting scope below
    SemanticContext::ScopeSwitchGuard definitionScope(
        ctx_, SemanticContext::definitionScopeOf(*genericInfo));
    SemanticContext::SourceFileGuard definitionFile(
        ctx_, genericInfo->AST->getSourceFileId());
    // Push a scope for type parameter bindings
    ctx_.enterTypeParamScope(typeParameterNames(genericInfo->typeParameters),
                             typeArgs);

    // Add fields with substituted types
    for (const auto& field : genericInfo->AST->getFields()) {
      auto fieldType = sema_.types().typeAnnotationToType(field.type);
      specializedInterface->addField(field.name, fieldType).visibility =
          field.visibility;
    }

    // Add methods with substituted types
    for (const auto& methodDecl : genericInfo->AST->getMethods()) {
      const PrototypeAST& proto = methodDecl.function->getProto();

      // Get return type with substitution
      sun::TypePtr returnType;
      if (proto.getReturnType()) {
        returnType = sema_.types().typeAnnotationToType(*proto.getReturnType());
      } else {
        returnType = sun::Types::Void();
      }

      // Get parameter types with substitution
      std::vector<sun::TypePtr> paramTypes;
      for (const auto& [argName, argType] : proto.getArgs()) {
        paramTypes.push_back(sema_.types().typeAnnotationToType(argType));
      }

      // Add method to interface type (preserve method-level generic type
      // parameters)
      auto& method = specializedInterface->addMethod(
          proto.getName(), returnType, paramTypes, methodDecl.hasDefaultImpl,
          proto.getTypeParameterNames());
      method.visibility = methodVisibility(*methodDecl.function);
      method.isConst = methodDecl.isConst;
    }

    // Pop the scope
    ctx_.exitScope();
  }

  // Register the specialized interface
  ctx_.registerInterface(mangledName, specializedInterface);

  return specializedInterface;
}

// -------------------------------------------------------------------
// Generic enum instantiation (monomorphization, mirrors generic classes):
// resolve each variant's payload annotations under the type parameter
// bindings and register the specialized EnumType. Specializations are
// recorded on the template AST, like other generic ASTs.
// -------------------------------------------------------------------

std::shared_ptr<sun::EnumType> GenericSpecializer::instantiateGenericEnum(
    const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs) {
  auto* genericInfo = ctx_.lookupGenericEnum(baseName);
  if (!genericInfo || !genericInfo->AST) {
    return nullptr;
  }

  // Mangle from the template's registered name, not the spelling the caller
  // used: `std.Option<i32>` and `Option<i32>` name the same specialization
  // (generic classes derive their name the same way).
  const std::string& templateName = genericInfo->qualifiedName.baseName.empty()
                                        ? baseName
                                        : genericInfo->qualifiedName.baseName;
  std::string mangledName = sun::Types::mangleGenericClassName(
      genericInfo->qualifiedName.mangled(), typeArgs);
  if (ctx_.types()->hasEnum(mangledName)) {
    auto existing = ctx_.types()->getEnum(mangledName);
    ctx_.registerEnum(mangledName, existing);
    return existing;
  }

  if (typeArgs.size() != genericInfo->typeParameters.size()) {
    logAndThrowError("Generic enum '" + baseName + "' expects " +
                     std::to_string(genericInfo->typeParameters.size()) +
                     " type argument(s), got " +
                     std::to_string(typeArgs.size()));
  }
  checkTypeParameterConstraints(genericInfo->typeParameters, typeArgs,
                                "generic enum", baseName);

  // `Option<T>` with T still a type parameter is the template's own shape
  // rather than a specialization, exactly as for generic classes: it is what
  // `count<T>(v: ref Vec<T>)` names before anyone calls it. The shape is
  // built, because a template body resolves against it, but a payload that is
  // still a type parameter is not checked — what a `T` may carry is only
  // knowable once T is a type — and it is never recorded as something to
  // emit, since codegen lays out every specialization recorded on the
  // template and cannot lay out a type parameter.
  bool abstractShape = false;
  for (const auto& arg : typeArgs) {
    if (arg && arg->isTypeParameter()) {
      abstractShape = true;
      break;
    }
  }

  auto specialized = ctx_.types()->getEnum(mangledName);
  specialized->setGenericQualifiedName(genericInfo->qualifiedName);
  specialized->setBaseName(templateName);
  specialized->setGenericOrigin(templateName, typeArgs);
  specialized->visibility = genericInfo->AST->getVisibility();
  specialized->setQualifiedName(
      sun::QualifiedName(genericInfo->qualifiedName.scopePath,
                         sun::Types::mangleGenericClassName(
                             genericInfo->qualifiedName.baseName, typeArgs),
                         genericInfo->qualifiedName.modulePath));

  {
    // Payload annotations resolve in the enum's definition scope; the result
    // is registered in the requesting scope below
    SemanticContext::ScopeSwitchGuard definitionScope(
        ctx_, SemanticContext::definitionScopeOf(*genericInfo));
    SemanticContext::SourceFileGuard definitionFile(
        ctx_, genericInfo->AST->getSourceFileId());
    ctx_.enterTypeParamScope(typeParameterNames(genericInfo->typeParameters),
                             typeArgs);
    for (const auto& variant : genericInfo->AST->getVariants()) {
      specialized->addVariant(variant.name, variant.value);
      if (!variant.hasPayload()) continue;
      std::vector<sun::TypePtr> payloadTypes;
      for (const auto& annot : variant.payloadTypes) {
        auto payloadType = sema_.types().typeAnnotationToType(annot);
        if (!abstractShape) {
          sema_.validateEnumPayloadType(payloadType, specialized, variant.name,
                                        variant.location);
        }
        payloadTypes.push_back(std::move(payloadType));
      }
      specialized->setVariantPayloadTypes(variant.name,
                                          std::move(payloadTypes));
    }
    ctx_.exitScope();
  }

  ctx_.registerEnum(mangledName, specialized);
  // Record on the template AST (mirrors generic classes); codegen walks
  // these to build storage structs
  if (!abstractShape) {
    genericInfo->AST->addSpecialization(mangledName, specialized);
  }
  return specialized;
}

sun::TypePtr GenericSpecializer::genericFunctionSignature(
    const GenericFunctionInfo& genericInfo,
    const std::vector<sun::TypePtr>& typeArgs) {
  ctx_.enterTypeParamScope(typeParameterNames(genericInfo.typeParameters),
                           typeArgs);
  std::vector<sun::TypePtr> paramTypes;
  for (const auto& [name, annot] : genericInfo.params) {
    paramTypes.push_back(sema_.types().typeAnnotationToType(annot));
  }
  sun::TypePtr returnType =
      genericInfo.returnType
          ? sema_.types().typeAnnotationToType(*genericInfo.returnType)
          : sun::Types::Void();
  ctx_.exitScope();
  bool canThrow = genericInfo.AST && genericInfo.AST->getProto().canThrow();
  return sun::Types::Function(returnType, paramTypes, canThrow);
}

bool GenericSpecializer::templateStillAbstract(
    const GenericFunctionInfo& genericInfo,
    const std::vector<sun::TypePtr>& typeArgs) {
  if (std::any_of(typeArgs.begin(), typeArgs.end(),
                  sun::generics::mentionsTypeParameter)) {
    return true;
  }
  // A template with no type parameters of its own still cannot be
  // specialized while a type parameter it borrows from an enclosing generic
  // is unbound — `function build(args...: _params_of<T>)` inside `outer<T>`.
  const PrototypeAST* proto =
      genericInfo.AST ? &genericInfo.AST->getProto() : nullptr;
  if (!proto || !proto->hasVariadicTypeAnnotation()) return false;
  // `_params_of` is not a type of its own; what may still be abstract is what
  // it is applied to.
  const TypeAnnotation& annot = proto->getVariadicTypeAnnotation();
  if (annot.typeArguments.empty()) return false;

  ctx_.enterTypeParamScope(typeParameterNames(genericInfo.typeParameters),
                           typeArgs);
  bool abstract = false;
  for (const auto& arg : annot.typeArguments) {
    abstract = abstract || sun::generics::mentionsTypeParameter(
                               sema_.types().typeAnnotationToType(*arg));
  }
  ctx_.exitScope();
  return abstract;
}
