// semantic_analysis/classes.cpp — Class and generic class support

#include "error.h"
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
  currentScope->genericClasses[name] = info;
}

const GenericClassInfo* SemanticAnalyzer::lookupGenericClass(
    const std::string& name) const {
  return currentScope->lookupGenericClass(name);
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
  // Look up the generic class definition first
  auto* genericClassInfo = lookupGenericClass(baseName);

  // Look up the generic class definition
  if (!genericClassInfo || !genericClassInfo->AST) {
    llvm::errs() << "Error: Unknown generic class '" << baseName << "'\n";
    return nullptr;
  }

  // Verify type argument count matches
  if (typeArgs.size() != genericClassInfo->typeParameters.size()) {
    llvm::errs() << "Error: Generic class '" << baseName << "' expects "
                 << genericClassInfo->typeParameters.size()
                 << " type arguments, got " << typeArgs.size() << "\n";
    return nullptr;
  }

  // Construct the specialized QualifiedName from the generic's qualified name
  // with type arguments mangled into the base name
  sun::QualifiedName specializedQName;
  specializedQName.scopePath = genericClassInfo->qualifiedName.scopePath;
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

  // Push a scope for class-level type parameter bindings
  enterClassScope(specializedQName);
  addTypeParameterBindings(genericClassInfo->typeParameters, typeArgs);

  // Link definition scope so transitive dependencies (types from imports in the
  // generic's source file) are accessible during instantiation. This allows
  // ContiguousBuffer<T> to resolve Unique<i8> even when instantiated from
  // matrix.sun which doesn't directly import unique.sun.
  if (auto defScope = genericClassInfo->definitionScope.lock()) {
    // Only add if definition scope is not already an ancestor (avoids cycle)
    bool isAncestor = false;
    for (auto* s = currentScope->parent; s != nullptr; s = s->parent) {
      if (s == defScope.get()) {
        isAncestor = true;
        break;
      }
    }
    if (!isAncestor) {
      // Add the definition scope as an accessible import scope for type lookups
      currentScope->childModules["__definition__"] = defScope;
    }
  }

  // Add fields with substituted types (skip if type already exists or already
  // has fields from a previous instantiation in another scope)
  if (!astOnlyMode && specializedClass->getFields().empty()) {
    for (const auto& field : genericClassInfo->AST->getFields()) {
      auto fieldType = typeAnnotationToType(field.type);
      fieldType = substituteTypeParameters(fieldType);
      specializedClass->addField(field.name, fieldType);
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
      specializedClass->addMethod(proto.getName(), returnType, paramTypes,
                                  methodClone.isConstructor,
                                  proto.getTypeParameters());
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

    // Store specialization on the generic class AST for codegen access
    genericClassInfo->AST->addSpecialization(mangledName, specializedAST);

    // Restore old class context
    setCurrentClass(savedClass);
    exitScope();
    // Remove from "being instantiated" set now that we're done
    classesBeingInstantiated.erase(mangledName);
    return specializedClass;
  }

  // PASS 2: Analyze all cloned method bodies
  for (auto& methodClone : methodsClone) {
    FunctionAST* methodFunc = methodClone.function.get();
    const auto& proto = methodFunc->getProto();

    // Skip generic methods - they are analyzed when called with type args
    if (proto.isGeneric()) {
      continue;
    }

    // Use the unified helper (type params already in outer scope, pass empty)
    // This analyzes the CLONED method, not the shared generic one
    analyzeMethodWithBindings(*methodFunc, specializedClass, {}, {});
  }

  // Create the specialized ClassDefinitionAST with cloned/analyzed methods
  // Note: Specializations are NOT precompiled - even if the generic class
  // came from a precompiled .moon file, new specializations with user-defined
  // type args (e.g., Unique<Point>) need codegen since they don't exist in
  // the library bitcode.
  auto specializedAST = std::make_shared<ClassDefinitionAST>(
      mangledName,                 // e.g., "Vec_i32" instead of "Vec"
      std::vector<std::string>{},  // empty - no longer generic
      std::move(interfacesClone), std::move(fieldsClone),
      std::move(methodsClone),  // cloned methods with analyzed bodies
      false);                   // NOT precompiled - needs codegen

  // Store specialization on the generic class AST for codegen access
  genericClassInfo->AST->addSpecialization(mangledName, specializedAST);

  // Restore old class context
  setCurrentClass(savedClass);

  // Pop the class-level type parameter scope
  exitScope();

  // Remove from "being instantiated" set now that we're done
  classesBeingInstantiated.erase(mangledName);

  return specializedClass;
}

// -------------------------------------------------------------------
// Generic function support
// -------------------------------------------------------------------

std::optional<SpecializedFunctionInfo>
SemanticAnalyzer::instantiateGenericFunction(
    const FunctionAST* genericFunc, const std::vector<sun::TypePtr>& typeArgs) {
  if (!genericFunc) {
    return std::nullopt;
  }

  const PrototypeAST& proto = genericFunc->getProto();
  // Use qualified name to include enclosing function context (e.g.,
  // outer_i32_inner)
  std::string funcName = proto.getMangledName();
  const auto& typeParams = proto.getTypeParameters();

  // Generate mangled name for cache lookup
  std::string mangledName = funcName;
  for (const auto& typeArg : typeArgs) {
    mangledName += "_" + typeArg->toString();
  }

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
    llvm::errs() << "Error: Generic function '" << funcName << "' expects "
                 << typeParams.size() << " type arguments, got "
                 << typeArgs.size() << "\n";
    return std::nullopt;
  }

  // Enter scope and bind type parameters
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

  // Substitute capture types
  std::vector<Capture> substitutedCaptures;
  for (const auto& cap : proto.getCaptures()) {
    Capture subCap;
    subCap.name = cap.name;
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
                       proto.canThrow());

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
    const std::string& baseName = classType->getBaseGenericName();
    auto* genericInfo = lookupGenericClass(baseName);
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
    const std::string& baseName = classType->getBaseGenericName();
    auto* genericInfo = lookupGenericClass(baseName);
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

  // Enter scope and add all type bindings
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

  // Extract module path from class context for type resolution.
  // For specialized generic classes, look up the generic class definition's
  // qualified name to get the module path.
  std::vector<std::string> modulePath;
  int moduleScopesEntered = 0;
  if (classType->isSpecialized()) {
    const std::string& baseName = classType->getBaseGenericName();
    auto* genericInfo = lookupGenericClass(baseName);
    if (genericInfo && !genericInfo->qualifiedName.scopePath.empty()) {
      modulePath = genericInfo->qualifiedName.scopePath;
    }
  }
  if (modulePath.empty()) {
    const std::string& className = classType->getMangledName();
    size_t underscorePos = className.find('_');
    if (underscorePos != std::string::npos) {
      modulePath.push_back(className.substr(0, underscorePos));
    }
  }

  // Enter module scope if class is from a namespace (for correct type
  // resolution)
  for (const auto& segment : modulePath) {
    enterModuleScope(segment);
    moduleScopesEntered++;
  }
  if (!modulePath.empty()) {
    // Add implicit using import for the module so unqualified names resolve
    addUsingImport(UsingImport(sun::QualifiedName::joinPath(modulePath), "*"));
  }

  // Compute method signature for nested function qualification
  std::string methodSig = getFunctionSignature(mangledName, paramTypes);

  // Enter method scope and declare parameters
  enterFunctionScope(methodSig, sun::QualifiedName(modulePath, mangledName),
                     proto.canThrow());

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
  for (int i = 0; i < moduleScopesEntered; ++i) {
    exitScope();  // module scope(s)
  }
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
