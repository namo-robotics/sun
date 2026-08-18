// semantic_analysis/classes.cpp — Class and generic class support

#include <algorithm>

#include "error.h"
#include "packed_layout.h"
#include "semantic_analyzer.h"

// -------------------------------------------------------------------
// Class support
// -------------------------------------------------------------------

void SemanticAnalyzer::registerClass(const std::string& name,
                                     std::shared_ptr<sun::ClassType> classType,
                                     std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (currentScope->classes.contains(name)) {
    return;
  }
  // Register in current scope
  currentScope->classes[name] = classType;
}

std::shared_ptr<sun::ClassType> SemanticAnalyzer::lookupClass(
    const std::string& name) const {
  return currentScope->lookupClass(name);
}

void SemanticAnalyzer::setCurrentClass(
    std::shared_ptr<sun::ClassType> classType) {
  currentClass = std::move(classType);
}

std::shared_ptr<sun::ClassType> SemanticAnalyzer::getCurrentClass() const {
  return currentClass;
}

// -------------------------------------------------------------------
// Packed class rules
// -------------------------------------------------------------------

// `ref p.field` would hand out an address the borrower accesses at the field
// type's natural alignment, which a packed field does not satisfy.
void SemanticAnalyzer::checkPackedFieldNotBorrowed(const ExprAST& target,
                                                   const Position& loc) const {
  if (target.getType() != ASTNodeType::MEMBER_ACCESS) return;
  std::string ownerName;
  if (!sun::packed::isFieldAccess(target, &ownerName)) return;
  logAndThrowError(
      sun::packed::borrowRejection(
          "create a reference to a " + sun::packed::fieldPhrase(ownerName),
          "Copy the field into a local instead."),
      loc);
}

// A ref parameter takes the argument's address, so it has the same problem.
void SemanticAnalyzer::checkPackedRefArguments(
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::TypePtr>& paramTypes) const {
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!paramTypes[i] || !paramTypes[i]->isReference()) continue;
    if (args[i]->getType() != ASTNodeType::MEMBER_ACCESS) continue;
    std::string ownerName;
    if (sun::packed::isFieldAccess(*args[i], &ownerName)) {
      logAndThrowError(
          sun::packed::borrowRejection(
              "pass a " + sun::packed::fieldPhrase(ownerName) +
                  " to a ref parameter",
              "Pass a copy instead."),
          args[i]->getLocation());
    }
  }
}

// Not every field type can live in a padding-free layout.
void SemanticAnalyzer::checkPackedFieldType(
    const ClassDefinitionAST& classDef, const ClassFieldDecl& field,
    const sun::TypePtr& fieldType) const {
  if (!classDef.isPacked()) return;
  std::string reason = sun::packed::rejectFieldType(fieldType);
  if (reason.empty()) return;
  logAndThrowError("Field '" + field.name + "' in packed class '" +
                       classDef.getName() + "' " + reason,
                   field.location);
}

// -------------------------------------------------------------------
// Generic class support
// -------------------------------------------------------------------

void SemanticAnalyzer::registerGenericClass(const std::string& name,
                                            const GenericClassInfo& info,
                                            std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (currentScope->genericClasses.contains(name)) {
    return;
  }
  // Register in current scope
  auto& slot = currentScope->genericClasses[name];
  slot = info;
  slot.definitionScope = currentScope->shared_from_this();
}

const GenericClassInfo* SemanticAnalyzer::lookupGenericClass(
    const std::string& name) const {
  return currentScope->lookupGenericClass(name);
}

const GenericClassInfo* SemanticAnalyzer::lookupGenericClass(
    const sun::QualifiedName& qualifiedName) const {
  return currentScope->lookupGenericClass(qualifiedName);
}

const GenericClassInfo* SemanticAnalyzer::lookupGenericClassOf(
    const sun::ClassType& specialized) const {
  return lookupGenericClass(specialized.getGenericQualifiedName());
}

// Scope a class's template was declared in: for a specialization, the
// generic's; for a plain class with generic methods, its own registration
// (hasGenericMethods() registers a GenericClassInfo too). nullptr if unknown.
SemanticScope* SemanticAnalyzer::classDefinitionScope(
    const sun::ClassType& classType) const {
  const GenericClassInfo* info =
      classType.isSpecialized() ? lookupGenericClassOf(classType)
                                : lookupGenericClass(classType.getQualifiedName());
  return info ? definitionScopeOf(*info) : nullptr;
}

void SemanticAnalyzer::addTypeParameterBindings(
    const std::vector<std::string>& params,
    const std::vector<sun::TypePtr>& args) {
  auto& scope = *currentScope;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    scope.typeParameters[params[i]] = args[i];
  }
}

sun::TypePtr SemanticAnalyzer::findTypeParameter(
    const std::string& name) const {
  // Search from innermost to outermost scope
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    auto found = s->typeParameters.find(name);
    if (found != s->typeParameters.end()) {
      return found->second;
    }
  }
  return nullptr;
}

sun::TypePtr SemanticAnalyzer::findTypeAlias(const std::string& name) const {
  // Search from innermost to outermost scope
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    auto found = s->typeAliases.find(name);
    if (found != s->typeAliases.end()) {
      return found->second;
    }
  }
  return nullptr;
}

// Instantiate a generic class with specific type arguments
std::shared_ptr<sun::ClassType> SemanticAnalyzer::instantiateGenericClass(
    const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs) {
  auto* genericClassInfo = lookupGenericClass(baseName);
  if (!genericClassInfo || !genericClassInfo->AST) {
    logAndThrowError("Unknown generic class '" + baseName + "'");
  }
  return instantiateGenericClass(*genericClassInfo, typeArgs);
}

std::shared_ptr<sun::ClassType> SemanticAnalyzer::instantiateGenericClass(
    const GenericClassInfo& info, const std::vector<sun::TypePtr>& typeArgs) {
  const GenericClassInfo* genericClassInfo = &info;
  const std::string& baseName = info.qualifiedName.baseName;

  // Verify type argument count matches
  if (typeArgs.size() != genericClassInfo->typeParameters.size()) {
    logAndThrowError(
        "Generic class '" + baseName + "' expects " +
        std::to_string(genericClassInfo->typeParameters.size()) +
        " type arguments, got " + std::to_string(typeArgs.size()));
  }

  // Construct the specialized QualifiedName from the generic's qualified name
  // with type arguments mangled into the base name
  sun::QualifiedName specializedQName;
  specializedQName.scopePath = genericClassInfo->qualifiedName.scopePath;
  specializedQName.modulePath = genericClassInfo->qualifiedName.modulePath;
  std::string specializedBaseName = genericClassInfo->qualifiedName.baseName;
  for (const auto& arg : typeArgs) {
    specializedBaseName += "_" + sun::Types::mangleTypeName(arg);
  }
  specializedQName.baseName = specializedBaseName;

  // Derive the mangled name from the qualified name
  std::string mangledName = specializedQName.mangled();

  // Check if already instantiated (both class type AND AST specialization)
  auto existing = lookupClass(specializedQName.baseName);
  if (existing && genericClassInfo->AST->hasSpecialization(mangledName)) {
    // Both type and AST exist - nothing more to do
    return existing;
  }

  // Check if we're already in the process of instantiating this class
  // (breaks mutual recursion like Vec<T> <-> VecIterator<T>)
  if (classesBeingInstantiated.count(mangledName)) {
    // Return the partially-created class type if it exists, or create a
    // placeholder This allows mutual references to be resolved
    if (existing) {
      return existing;
    }
    // Create and register a placeholder class type
    auto placeholder = typeRegistry->getClass(specializedQName);
    registerClass(specializedQName.baseName, placeholder);
    return placeholder;
  }

  // Track if we're only creating the AST (type already exists but AST doesn't)
  // This can happen when type is resolved (e.g., for a field type) before
  // a 'declare' statement explicitly instantiates it
  bool astOnlyMode = (existing != nullptr);

  // Mark this class as being instantiated to break mutual recursion
  classesBeingInstantiated.insert(mangledName);

  // Create or reuse the specialized class type
  std::shared_ptr<sun::ClassType> specializedClass;
  if (astOnlyMode) {
    specializedClass = existing;
  } else {
    specializedClass = typeRegistry->getClass(specializedQName);

    // Set type arguments for specialized class tracking
    // (getClass sets qualifiedName and baseName, but not type args)
    if (!specializedClass->isSpecialized()) {
      // This is a new class type - need to configure it as specialized
      // Get or create via getSpecializedClass for proper setup
      specializedClass = typeRegistry->getSpecializedClass(
          genericClassInfo->qualifiedName.mangled(), typeArgs);
      specializedClass->setQualifiedName(specializedQName);
      specializedClass->setGenericQualifiedName(genericClassInfo->qualifiedName);
    }

    // Register the specialized class so methods can reference it
    registerClass(specializedQName.baseName, specializedClass);

    // If this class was already fully instantiated in another scope
    // (fields populated + specialization AST created), just register in
    // current scope and return - avoids duplicating fields/methods/interfaces
    if (!specializedClass->getFields().empty() &&
        genericClassInfo->AST->hasSpecialization(mangledName)) {
      classesBeingInstantiated.erase(mangledName);
      return specializedClass;
    }
  }

  // The template's members, interfaces and bodies are analyzed in the scope
  // the template was declared in, wherever this instantiation was requested
  // from: names resolve as they do at the definition site (transitive
  // dependencies of its module included) and access control sees the
  // template's own module.
  ScopeSwitchGuard definitionScope(*this,
                                   definitionScopeOf(*genericClassInfo));

  // Push a scope for class-level type parameter bindings
  enterClassScope(specializedQName);
  addTypeParameterBindings(genericClassInfo->typeParameters, typeArgs);

  // Layout is a property of the generic definition, so every specialization
  // inherits it. Must precede the first getStructType(), which memoizes.
  specializedClass->setPacked(genericClassInfo->AST->isPacked());
  specializedClass->visibility = genericClassInfo->AST->getVisibility();

  // Add fields with substituted types (skip if type already exists or already
  // has fields from a previous instantiation in another scope)
  if (!astOnlyMode && specializedClass->getFields().empty()) {
    for (const auto& field : genericClassInfo->AST->getFields()) {
      auto fieldType = typeAnnotationToType(field.type);
      fieldType = substituteTypeParameters(fieldType);
      // Checked per specialization: whether a type argument is packable is
      // only knowable once T is substituted
      checkPackedFieldType(*genericClassInfo->AST, field, fieldType);
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
          auto argType = typeAnnotationToType(typeArg);
          argType = substituteTypeParameters(argType);
          ifaceTypeArgs.push_back(argType);
        }
        interfaceType =
            instantiateGenericInterface(ifaceRef.name, ifaceTypeArgs);
      } else {
        interfaceType = lookupInterface(ifaceRef.name);
      }

      // Add interface to class type
      if (interfaceType) {
        specializedClass->addImplementedInterface(interfaceType->getName());
      }
    }

    // Clone interface reference for specialized AST
    ImplementedInterfaceAST ifaceClone;
    ifaceClone.name = ifaceRef.name;
    for (const auto& ta : ifaceRef.typeArguments) {
      ifaceClone.typeArguments.push_back(ta);
    }
    interfacesClone.push_back(std::move(ifaceClone));
  }

  // Save old class context and set new one for method analysis
  auto savedClass = currentClass;
  setCurrentClass(specializedClass);

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
    methodsClone.push_back(std::move(methodClone));
  }

  // PASS 1: Register all methods first (so methods can call each other)
  // In astOnlyMode, we skip type-system registration but still resolve types
  // for the cloned method prototypes
  for (size_t i = 0; i < methodsClone.size(); ++i) {
    const auto& methodClone = methodsClone[i];
    const auto& proto = methodClone.function->getProto();

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

    // Add method to class type (skip if type already exists)
    if (!astOnlyMode) {
      specializedClass
          ->addMethod(proto.getName(), returnType, paramTypes,
                      methodClone.isConstructor, proto.getTypeParameters(),
                      proto.canThrow())
          .visibility = methodVisibility(*methodClone.function);
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
      bindings.emplace_back(genericClassInfo->typeParameters[i], typeArgs[i]);
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
      registernFunctionInCurrentScope(methodMangledName,
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
        mangledName, std::vector<std::string>{}, std::move(interfacesClone),
        std::move(fieldsClone), std::move(methodsClone),
        genericClassInfo->AST->isPrecompiled());
    specializedAST->setIsPacked(genericClassInfo->AST->isPacked());
    specializedAST->setVisibility(genericClassInfo->AST->getVisibility());

    // Store specialization on the generic class AST for codegen access
    genericClassInfo->AST->addSpecialization(mangledName, specializedAST);

    // Restore old class context
    setCurrentClass(savedClass);
    exitScope();
    // Remove from "being instantiated" set now that we're done
    classesBeingInstantiated.erase(mangledName);
    return specializedClass;
  }

  // Create the specialized ClassDefinitionAST with the cloned methods (bodies
  // analyzed below, through this AST).
  // Note: Specializations are NOT precompiled - even if the generic class
  // came from a precompiled .moon file, new specializations with user-defined
  // type args (e.g., Unique<Point>) need codegen since they don't exist in
  // the library bitcode.
  auto specializedAST = std::make_shared<ClassDefinitionAST>(
      mangledName,                 // e.g., "Vec_i32" instead of "Vec"
      std::vector<std::string>{},  // empty - no longer generic
      std::move(interfacesClone), std::move(fieldsClone),
      std::move(methodsClone),  // cloned methods
      false);                   // NOT precompiled - needs codegen
  specializedAST->setIsPacked(genericClassInfo->AST->isPacked());
  specializedAST->setVisibility(genericClassInfo->AST->getVisibility());
  specializedAST->setLocation(genericClassInfo->AST->getLocation());

  // Interface conformance is checked per specialization (signatures are
  // only known once T is substituted)
  if (!astOnlyMode) {
    validateInterfaceImplementation(*specializedAST, specializedClass);
  }

  // Store specialization on the generic class AST for codegen access
  genericClassInfo->AST->addSpecialization(mangledName, specializedAST);

  // PASS 2: Analyze all cloned method bodies — unless requested from the
  // declaration pre-pass, where bodies are deferred until every declaration
  // (including functions the bodies may call) is registered.
  bool deferBodies = declarationPrepassDepth_ > 0;
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
      analyzeMethodWithBindings(*methodFunc, specializedClass, {}, {});
    }
  }

  if (deferBodies) {
    deferredSpecializations_.push_back(
        {specializedClass, genericClassInfo, typeArgs, specializedAST});
  }

  // Restore old class context
  setCurrentClass(savedClass);

  // Pop the class-level type parameter scope
  exitScope();

  // Remove from "being instantiated" set now that we're done
  classesBeingInstantiated.erase(mangledName);

  return specializedClass;
}

// Analyze the method bodies of specializations created during the
// declaration pre-pass. Re-enters an equivalent class scope (type parameter
// bindings) inside the template's definition scope. Bodies may create
// further specializations; those are analyzed immediately (the pre-pass is
// over) so the loop is by index.
void SemanticAnalyzer::analyzeDeferredSpecializations() {
  for (size_t i = 0; i < deferredSpecializations_.size(); ++i) {
    DeferredSpecialization d = deferredSpecializations_[i];
    ScopeSwitchGuard definitionScope(*this,
                                     definitionScopeOf(*d.genericInfo));

    enterClassScope(d.specializedClass->getQualifiedName());
    addTypeParameterBindings(d.genericInfo->typeParameters, d.typeArgs);
    auto savedClass = currentClass;
    setCurrentClass(d.specializedClass);

    for (auto& methodClone : d.specializedAST->getMutableMethods()) {
      FunctionAST* methodFunc = methodClone.function.get();
      if (methodFunc->getProto().isGeneric()) continue;
      analyzeMethodWithBindings(*methodFunc, d.specializedClass, {}, {});
    }

    setCurrentClass(savedClass);
    exitScope();
  }
  deferredSpecializations_.clear();
}

// -------------------------------------------------------------------
// Generic function support
// -------------------------------------------------------------------

namespace {

// Match one parameter annotation against the type of the argument it
// receives, binding any type parameter it names. Only shapes a reader can
// follow are matched: the bare parameter, a `ref`, a pointer or array
// element, and the type arguments of a generic type.
void bindTypeParameters(
    const TypeAnnotation& annot, const sun::TypePtr& argType,
    const std::vector<std::string>& typeParams,
    std::map<std::string, sun::TypePtr>& bindings) {
  if (!argType) return;

  auto isTypeParam = [&](const std::string& name) {
    return std::find(typeParams.begin(), typeParams.end(), name) !=
           typeParams.end();
  };

  // `ref T` against an argument of type T (or ref T)
  if (annot.baseName == "ref" && annot.elementType) {
    sun::TypePtr referent = argType;
    if (argType->isReference()) {
      referent =
          static_cast<const sun::ReferenceType*>(argType.get())
              ->getReferencedType();
    }
    bindTypeParameters(*annot.elementType, referent, typeParams, bindings);
    return;
  }

  // Reading through a borrow gives the referent's type everywhere else
  sun::TypePtr value = argType;
  if (value->isReference()) {
    value = static_cast<const sun::ReferenceType*>(value.get())
                ->getReferencedType();
  }
  if (!value) return;

  // The parameter is the type parameter itself: T, bound to the argument
  if (annot.typeArguments.empty() && isTypeParam(annot.baseName)) {
    auto existing = bindings.find(annot.baseName);
    if (existing == bindings.end()) bindings[annot.baseName] = value;
    return;
  }

  // array<T> / raw_ptr<T> / static_ptr<T>: bind against the element
  if (annot.elementType) {
    sun::TypePtr element;
    if (value->isArray()) {
      element = static_cast<const sun::ArrayType*>(value.get())
                    ->getElementType();
    } else if (value->isRawPointer()) {
      element = static_cast<const sun::RawPointerType*>(value.get())
                    ->getPointeeType();
    } else if (value->isStaticPointer()) {
      element = static_cast<const sun::StaticPointerType*>(value.get())
                    ->getPointeeType();
    }
    if (element) {
      bindTypeParameters(*annot.elementType, element, typeParams, bindings);
    }
    return;
  }

  // Vec<T>, Map<K, V>, Option<T>: bind against the argument's type arguments
  if (!annot.typeArguments.empty()) {
    const std::vector<sun::TypePtr>* args = nullptr;
    if (value->isClass()) {
      args = &static_cast<const sun::ClassType*>(value.get())
                  ->getTypeArguments();
    } else if (value->isEnum()) {
      args =
          &static_cast<const sun::EnumType*>(value.get())->getGenericArgs();
    }
    if (!args) return;
    for (size_t i = 0; i < annot.typeArguments.size() && i < args->size();
         ++i) {
      bindTypeParameters(*annot.typeArguments[i], (*args)[i], typeParams,
                         bindings);
    }
  }
}

}  // namespace

// Work out `f<...>` from the arguments of a call written without them.
std::vector<sun::TypePtr> SemanticAnalyzer::inferGenericTypeArguments(
    const GenericFunctionInfo& genericInfo,
    const std::vector<sun::TypePtr>& argTypes, const std::string& displayName,
    std::optional<Position> loc) const {
  std::map<std::string, sun::TypePtr> bindings;
  for (size_t i = 0; i < genericInfo.params.size() && i < argTypes.size();
       ++i) {
    bindTypeParameters(genericInfo.params[i].second, argTypes[i],
                       genericInfo.typeParameters, bindings);
  }

  std::vector<sun::TypePtr> typeArgs;
  for (const auto& typeParam : genericInfo.typeParameters) {
    auto found = bindings.find(typeParam);
    if (found == bindings.end() || !found->second ||
        found->second->isTypeParameter()) {
      logAndThrowError("Cannot infer type argument '" + typeParam +
                           "' of generic function '" + displayName +
                           "' from the arguments. Give it explicitly, e.g. " +
                           displayName + "<i32>(...).",
                       loc);
    }
    typeArgs.push_back(found->second);
  }
  return typeArgs;
}

SpecializedFunctionInfo SemanticAnalyzer::requireGenericSpecialization(
    const GenericFunctionInfo& genericInfo,
    const std::vector<sun::TypePtr>& typeArgs, const std::string& displayName,
    std::optional<Position> loc) {
  auto specialized = instantiateGenericFunction(genericInfo, typeArgs);
  if (!specialized) {
    logAndThrowError("Failed to instantiate generic function '" + displayName +
                         "' with the given type arguments",
                     loc);
  }
  return *specialized;
}

std::optional<SpecializedFunctionInfo>
SemanticAnalyzer::instantiateGenericFunction(
    const GenericFunctionInfo& genericInfo,
    const std::vector<sun::TypePtr>& typeArgs) {
  const FunctionAST* genericFunc = genericInfo.AST;
  if (!genericFunc) {
    return std::nullopt;
  }
  const PrototypeAST& proto = genericFunc->getProto();
  // Name the specialization off the template's registration, which includes
  // enclosing function context (e.g. outer_i32_inner) and is fixed from the
  // moment the template is collected. The prototype's own mangled name only
  // gains its overload suffix once its definition is analyzed, so using it
  // would name the same specialization differently depending on whether the
  // call site sits above or below the definition.
  std::string funcName = genericInfo.qualifiedName.empty()
                             ? proto.getMangledName()
                             : genericInfo.qualifiedName.mangled();
  const auto& typeParams = proto.getTypeParameters();

  // Generate mangled name for cache lookup
  std::string mangledName = funcName;
  for (const auto& typeArg : typeArgs) {
    mangledName += "_" + typeArg->toString();
  }
  // The name the specialization is emitted under. Type arguments are part of
  // the name itself, so it carries no scope path or overload suffix — this is
  // the same name the cloned prototype gets below.
  const sun::QualifiedName specializedName({}, mangledName);

  // Check cache first
  auto cacheIt = specializedFunctionCache.find(mangledName);
  if (cacheIt != specializedFunctionCache.end()) {
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

  // Enter scope and bind type parameters
  // Analyze the body in the scope the template was declared in (a module,
  // or the enclosing specialized function for nested generics — that is
  // where its captures and outer type bindings live)
  ScopeSwitchGuard definitionScope(*this, definitionScopeOf(genericInfo));
  enterTypeParamScope(typeParams, typeArgs);

  // Substitute parameter types
  std::vector<sun::TypePtr> paramTypes;
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);
    paramType = substituteTypeParameters(paramType);

    // If a type parameter resolved to a compound type, error - must use ref
    if (paramType && paramType->isCompound() && !paramType->isReference()) {
      logAndThrowError("Parameter '" + argName + "' has compound type '" +
                           paramType->toString() +
                           "' which cannot be passed by value. Use 'ref " +
                           paramType->toString() + "' instead.",
                       genericFunc->getLocation());
    }

    paramTypes.push_back(paramType);
  }

  // Substitute return type
  sun::TypePtr returnType;
  if (proto.hasReturnType()) {
    returnType = typeAnnotationToType(*proto.getReturnType());
    returnType = substituteTypeParameters(returnType);
  } else {
    returnType = sun::Types::Void();
  }

  // Substitute capture types (field-by-field rebuild; keep byRef intact)
  std::vector<Capture> substitutedCaptures;
  for (const auto& cap : proto.getCaptures()) {
    Capture subCap = cap;
    subCap.type = substituteTypeParameters(cap.type);
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
    clonedProto.setName(mangledName);
    // Update qualified name to the mangled name so nested functions get correct
    // scopePath (e.g., outer_i32 instead of outer)
    clonedProto.setQualifiedName(sun::QualifiedName({}, mangledName));

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

    // Clear resolved types for fresh analysis
    clearResolvedTypes(*clonedFunc);

    // Compute function signature for nested function qualification
    std::string funcSig = getFunctionSignature(mangledName, paramTypes);

    // Declare parameters in scope for body analysis - use the mangled qualified
    // name so nested functions get correct context
    enterFunctionScope(funcSig, clonedProto.getQualifiedName(),
                       proto.canThrow(),
                       clonedProto.getResolvedReturnType());

    // Record the variadic pack on the function scope (see method path). Today
    // the function path never resolves variadic types, so this is a no-op until
    // generic-function variadics are supported.
    if (clonedProto.hasVariadicParam() &&
        clonedProto.hasResolvedVariadicTypes()) {
      if (auto* fnScope = currentFunctionScope()) {
        fnScope->variadicParam = {*clonedProto.getVariadicParamName(),
                                  clonedProto.getResolvedVariadicTypes()};
      }
    }

    for (size_t i = 0; i < paramTypes.size(); ++i) {
      // Use parameter names from the cloned prototype
      std::string argName = proto.getArgs()[i].first;
      declareVariable(argName, paramTypes[i], /*isParam=*/true);
    }

    // Add captures to scope
    for (const auto& cap : substitutedCaptures) {
      declareVariable(cap.name, cap.type);
    }

    // Analyze the body with current type parameter bindings
    analyzeBlock(const_cast<BlockExprAST&>(clonedFunc->getBody()));

    exitScope();  // parameter scope

    specializedAST = std::move(clonedFunc);
    // Specializations are NOT precompiled - even if the generic function
    // came from a precompiled .moon file, new specializations need codegen
    // since they don't exist in the library bitcode.
    specializedAST->setPrecompiled(false);
  }

  exitScope();  // type parameter scope

  // Build result
  SpecializedFunctionInfo result;
  result.qualifiedName = specializedName;
  result.returnType = returnType;
  result.paramTypes = std::move(paramTypes);
  result.captures = std::move(substitutedCaptures);
  result.specializedAST = specializedAST;

  // Store specialization on the generic function AST for codegen access
  if (specializedAST) {
    genericFunc->addSpecialization(mangledName, specializedAST);
  }

  // Cache and return
  specializedFunctionCache[mangledName] = result;
  return result;
}

// -------------------------------------------------------------------
// Generic method instantiation
// -------------------------------------------------------------------

// Find the generic method's FunctionAST on a class by name. Resolves the class
// definition (specialized AST when available, else the generic definition) and
// returns the first generic method matching `methodName`, or nullptr.
FunctionAST* SemanticAnalyzer::findGenericMethodAST(
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
    auto* genericInfo = lookupGenericClass(classType->getBaseName());
    if (genericInfo) classDef = genericInfo->AST;
  } else {
    // Non-generic class - may still have generic methods
    auto* genericInfo = lookupGenericClass(classType->getBaseName());
    if (genericInfo) classDef = genericInfo->AST;
  }

  if (!classDef) return nullptr;

  for (const auto& methodDecl : classDef->getMethods()) {
    if (methodDecl.function->getProto().getName() == methodName &&
        methodDecl.function->getProto().isGeneric()) {
      return methodDecl.function.get();
    }
  }
  return nullptr;
}

std::shared_ptr<FunctionAST> SemanticAnalyzer::instantiateGenericMethod(
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

  // Build the specialized mangled name
  // Format: ClassName_methodName_TypeArg1_TypeArg2...[$v$argType1$argType2...]
  // The variadic suffix keys the specialization on the actual variadic argument
  // types so overloaded factories (e.g. create<Point>(7) vs create<Point>(3,4))
  // get distinct specializations. Codegen rebuilds this identically.
  std::string baseMangledName = classType->getMangledMethodName(methodName);
  std::string mangledName = baseMangledName;
  for (const auto& typeArg : methodTypeArgs) {
    mangledName += "_" + typeArg->toString();
  }
  if (variadicArgTypes) {
    std::string hashPrefix =
        sun::QualifiedName::extractHashPrefix(classType->getMangledName());
    mangledName +=
        sun::QualifiedName::buildVariadicArgSuffix(*variadicArgTypes, hashPrefix);
  }

  // Check cache
  auto cacheIt = specializedFunctionCache.find(mangledName);
  if (cacheIt != specializedFunctionCache.end() &&
      cacheIt->second.specializedAST) {
    return cacheIt->second.specializedAST;
  }

  // Find the method's FunctionAST from the class definition
  FunctionAST* genericMethodAST = findGenericMethodAST(classType.get(),
                                                       methodName);
  if (!genericMethodAST) {
    return nullptr;  // Generic method not found
  }

  const PrototypeAST& proto = genericMethodAST->getProto();
  const auto& methodTypeParams = proto.getTypeParameters();

  // A variadic method's arity/types come from the actual call arguments. If we
  // weren't given them (nullopt, e.g. invoked from type inference), defer: the
  // call-site trigger will specialize with the real variadic arg types.
  if (proto.hasVariadicConstraint() && !variadicArgTypes) {
    return nullptr;
  }

  if (methodTypeArgs.size() != methodTypeParams.size()) {
    logAndThrowError(
        "Type argument count mismatch for generic method " + methodName,
        genericMethodAST->getLocation());
    return nullptr;
  }

  // Set up scopes for type substitution:
  // 1. Class-level type parameters (if specialized generic class)
  // 2. Method-level type parameters

  std::vector<std::string> allTypeParams;
  std::vector<sun::TypePtr> allTypeArgs;

  // Collect class-level type parameter bindings for specialized generic classes
  if (classType->isSpecialized()) {
    auto* genericInfo = lookupGenericClassOf(*classType);
    if (genericInfo) {
      const auto& classTypeParams = genericInfo->typeParameters;
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
    allTypeParams.push_back(methodTypeParams[i]);
    allTypeArgs.push_back(methodTypeArgs[i]);
  }

  // Enter scope and add all type bindings, inside the class's definition
  // scope so the body resolves names as written at the definition site
  ScopeSwitchGuard definitionScope(*this, classDefinitionScope(*classType));
  enterTypeParamScope(allTypeParams, allTypeArgs);

  // Substitute types in parameters
  std::vector<sun::TypePtr> paramTypes;
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);
    paramType = substituteTypeParameters(paramType);
    paramTypes.push_back(paramType);
  }

  // Substitute return type
  sun::TypePtr returnType = proto.hasReturnType()
                                ? typeAnnotationToType(*proto.getReturnType())
                                : sun::Types::Void();
  returnType = substituteTypeParameters(returnType);

  // Clone the function AST for specialization
  // clone() returns unique_ptr<ExprAST>, so cast to FunctionAST
  auto cloned = genericMethodAST->clone();
  auto clonedFunc =
      std::unique_ptr<FunctionAST>(static_cast<FunctionAST*>(cloned.release()));

  PrototypeAST& clonedProto = const_cast<PrototypeAST&>(clonedFunc->getProto());

  // Set the specialized name on the prototype
  clonedProto.setName(mangledName);

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

  // Handle variadic constraint: _init_args<T>. The variadic arity/types are
  // driven by the ACTUAL call arguments (variadicArgTypes), so that overloaded
  // constructors are supported: create<Point>(7) selects init(i32) while
  // create<Point>(3,4) selects init(i32,i32) from the same create<Point>. We
  // only validate that T actually has a matching init overload here; the
  // matching init is selected downstream at _init via lookupConstructor.
  if (proto.hasVariadicConstraint() && variadicArgTypes) {
    clonedProto.setResolvedVariadicTypes(*variadicArgTypes);

    const auto& constraint = *proto.getVariadicConstraint();
    bool isInitArgs =
        (constraint.baseName == "_init_args" ||
         constraint.baseName.find("_init_args") != std::string::npos);
    if (isInitArgs && !constraint.typeArguments.empty()) {
      sun::TypePtr constraintType =
          typeAnnotationToType(*constraint.typeArguments[0]);
      constraintType = substituteTypeParameters(constraintType);

      if (constraintType && constraintType->isClass()) {
        auto* targetClass = static_cast<sun::ClassType*>(constraintType.get());
        // Validate that some init overload matches the call's argument types.
        if (targetClass->getMethod("init") &&
            !targetClass->getMethodForArgs("init", *variadicArgTypes)) {
          std::string argList;
          for (size_t i = 0; i < variadicArgTypes->size(); ++i) {
            if (i > 0) argList += ", ";
            argList += (*variadicArgTypes)[i]
                           ? (*variadicArgTypes)[i]->toDisplayString()
                           : "?";
          }
          logAndThrowError("No matching constructor for '" +
                               constraintType->toString() + "' with arguments (" +
                               argList + ")",
                           genericMethodAST->getLocation());
        }
      }
    }
  }

  // Clear any stale resolved types from previous specializations
  clearResolvedTypes(*clonedFunc);

  // Analyze the method body
  auto savedClass = currentClass;
  setCurrentClass(classType);

  // Compute method signature for nested function qualification
  std::string methodSig = getFunctionSignature(mangledName, paramTypes);

  // Enter method scope and declare parameters
  enterFunctionScope(methodSig,
                     sun::QualifiedName(classType->getQualifiedName().scopePath,
                                        mangledName),
                     proto.canThrow(), proto.getResolvedReturnType());

  // Record the variadic pack (name + resolved element types) on the function
  // scope so `args...` can be expanded into concrete typed args during body
  // analysis. Set it whenever the method has a variadic param, including the
  // zero-element case (an empty pack expands to no args). exitScope() discards
  // it.
  if (clonedProto.hasVariadicParam()) {
    if (auto* fnScope = currentFunctionScope()) {
      fnScope->variadicParam = {*clonedProto.getVariadicParamName(),
                                clonedProto.getResolvedVariadicTypes()};
    }
  }

  // Declare 'this' parameter
  declareVariable("this", classType, /*isParam=*/true);

  // Declare regular parameters
  for (size_t i = 0; i < paramTypes.size(); ++i) {
    const auto& [argName, argType] = proto.getArgs()[i];
    declareVariable(argName, paramTypes[i], /*isParam=*/true);
  }

  // Analyze the body
  analyzeBlock(const_cast<BlockExprAST&>(clonedFunc->getBody()));

  exitScope();  // method scope
  setCurrentClass(savedClass);
  exitScope();  // type parameter scope

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
  specializedFunctionCache[mangledName] = result;

  return specializedAST;
}  // End of instantiateGenericMethod
