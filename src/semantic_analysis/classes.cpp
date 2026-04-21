// semantic_analysis/classes.cpp — Class and generic class support

#include "error.h"
#include "parser.h"
#include "semantic_analyzer.h"

// -------------------------------------------------------------------
// Class support
// -------------------------------------------------------------------

void SemanticAnalyzer::registerClass(
    const std::string& name, std::shared_ptr<sun::ClassType> classType) {
  // Check for redeclaration in global scope
  if (!scopeStack.empty() && scopeStack.front().classes.contains(name)) {
    if (!collectingDeclarations) return;  // Pass 2: skip, already registered
    logAndThrowError("Cannot redeclare class '" + name + "'");
  }
  // Register in current scope AND global scope (for reachability)
  if (!scopeStack.empty()) {
    scopeStack.back().classes[name] = classType;
    if (scopeStack.size() > 1) {
      scopeStack.front().classes[name] = classType;
    }
  }
}

std::shared_ptr<sun::ClassType> SemanticAnalyzer::lookupClass(
    const std::string& name) const {
  // Walk scope chain from innermost to outermost, including child modules
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto result = it->findClass(name);
    if (result) return result;
  }
  return nullptr;
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
                                            const GenericClassInfo& info) {
  if (!scopeStack.empty() && scopeStack.front().genericClasses.contains(name)) {
    if (!collectingDeclarations) return;  // Pass 2: skip
    logAndThrowError("Cannot redeclare generic class '" + name + "'");
  }
  // Register in current scope AND global scope (for reachability)
  if (!scopeStack.empty()) {
    scopeStack.back().genericClasses[name] = info;
    if (scopeStack.size() > 1) {
      scopeStack.front().genericClasses[name] = info;
    }
  }
}

const GenericClassInfo* SemanticAnalyzer::lookupGenericClass(
    const std::string& name) const {
  // Walk scope chain from innermost to outermost, including child modules
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto result = it->findGenericClass(name);
    if (result) return result;
  }
  return nullptr;
}

void SemanticAnalyzer::addTypeParameterBindings(
    const std::vector<std::string>& params,
    const std::vector<sun::TypePtr>& args) {
  if (scopeStack.empty()) return;
  auto& scope = scopeStack.back();
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    scope.typeParameters[params[i]] = args[i];
  }
}

sun::TypePtr SemanticAnalyzer::findTypeParameter(
    const std::string& name) const {
  // Search from innermost to outermost scope
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto found = it->typeParameters.find(name);
    if (found != it->typeParameters.end()) {
      return found->second;
    }
  }
  return nullptr;
}

sun::TypePtr SemanticAnalyzer::findTypeAlias(const std::string& name) const {
  // Search from innermost to outermost scope
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto found = it->typeAliases.find(name);
    if (found != it->typeAliases.end()) {
      return found->second;
    }
  }
  return nullptr;
}

// Instantiate a generic class with specific type arguments
std::shared_ptr<sun::ClassType> SemanticAnalyzer::instantiateGenericClass(
    const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs) {
  // Generate mangled name for the specialized class
  std::string mangledName =
      sun::Types::mangleGenericClassName(baseName, typeArgs);

  // Check if already instantiated (both class type AND AST specialization)
  auto* genericClassInfo = lookupGenericClass(baseName);
  auto existing = lookupClass(mangledName);
  if (existing && genericClassInfo &&
      genericClassInfo->AST->hasSpecialization(mangledName)) {
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
    auto placeholder = typeRegistry->getSpecializedClass(baseName, typeArgs);
    registerClass(mangledName, placeholder);
    return placeholder;
  }

  // Look up the generic class definition
  if (!genericClassInfo || !genericClassInfo->AST) {
    llvm::errs() << "Error: Unknown generic class '" << baseName << "'\n";
    return nullptr;
  }

  // Track if we're only creating the AST (type already exists but AST doesn't)
  // This can happen when type is resolved (e.g., for a field type) before
  // a 'declare' statement explicitly instantiates it
  bool astOnlyMode = (existing != nullptr);

  // Mark this class as being instantiated to break mutual recursion
  classesBeingInstantiated.insert(mangledName);

  // Verify type argument count matches
  if (typeArgs.size() != genericClassInfo->typeParameters.size()) {
    llvm::errs() << "Error: Generic class '" << baseName << "' expects "
                 << genericClassInfo->typeParameters.size()
                 << " type arguments, got " << typeArgs.size() << "\n";
    return nullptr;
  }

  // Create or reuse the specialized class type
  std::shared_ptr<sun::ClassType> specializedClass;
  if (astOnlyMode) {
    specializedClass = existing;
  } else {
    specializedClass = typeRegistry->getSpecializedClass(baseName, typeArgs);
  }

  // Push a scope for class-level type parameter bindings
  enterScope(ScopeType::Class);
  addTypeParameterBindings(genericClassInfo->typeParameters, typeArgs);

  // Add fields with substituted types (skip if type already exists)
  if (!astOnlyMode) {
    for (const auto& field : genericClassInfo->AST->getFields()) {
      auto fieldType = typeAnnotationToType(field.type);
      fieldType = substituteTypeParameters(fieldType);
      specializedClass->addField(field.name, fieldType);
    }

    // Register the specialized class early so methods can reference it
    registerClass(mangledName, specializedClass);
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
      registerFunction(methodMangledName, {returnType, methodParamTypes, {}});
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
    lazyParseAndAnalyzeMethod(*methodFunc, specializedClass, {}, {});
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
  const std::string& funcName = proto.getName();
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
  enterScope();
  addTypeParameterBindings(typeParams, typeArgs);

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
                       paramType->toString() + "' instead.");
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

    // Declare parameters in scope for body analysis
    enterFunctionScope(funcSig);
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      const auto& [argName, argType] = proto.getArgs()[i];
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

std::shared_ptr<FunctionAST> SemanticAnalyzer::instantiateGenericMethod(
    std::shared_ptr<sun::ClassType> classType, const std::string& methodName,
    const std::vector<sun::TypePtr>& methodTypeArgs) {
  if (!classType || methodName.empty() || methodTypeArgs.empty()) {
    return nullptr;
  }

  // Build the specialized mangled name
  // Format: ClassName_methodName_TypeArg1_TypeArg2...
  std::string baseMangledName = classType->getMangledMethodName(methodName);
  std::string mangledName = baseMangledName;
  for (const auto& typeArg : methodTypeArgs) {
    mangledName += "_" + typeArg->toString();
  }

  // Check cache
  auto cacheIt = specializedFunctionCache.find(mangledName);
  if (cacheIt != specializedFunctionCache.end() &&
      cacheIt->second.specializedAST) {
    return cacheIt->second.specializedAST;
  }

  // Find the method's FunctionAST from the class definition
  // For specialized generic classes, find the base generic class
  const ClassDefinitionAST* classDef = nullptr;
  FunctionAST* genericMethodAST = nullptr;

  // For specialized classes, look up the method from the SPECIALIZED class AST
  // (not the base generic class), because that's what codegen will iterate over
  if (classType->isSpecialized()) {
    const std::string& baseName = classType->getBaseGenericName();
    auto* genericInfo = lookupGenericClass(baseName);
    if (genericInfo && genericInfo->AST) {
      // Get the specialized class AST - this is what codegen will process
      auto specAST = genericInfo->AST->getSpecialization(classType->getName());
      if (specAST) {
        classDef = specAST.get();
      } else {
        // Fallback to base definition if specialization not found yet
        classDef = genericInfo->AST;
      }
    }
  } else if (classType->isGenericDefinition()) {
    // Generic class definition - look up directly
    auto* genericInfo = lookupGenericClass(classType->getName());
    if (genericInfo) {
      classDef = genericInfo->AST;
    }
  } else {
    // Non-generic class - look up in generic table anyway (might have generic
    // methods)
    auto* genericInfo = lookupGenericClass(classType->getName());
    if (genericInfo) {
      classDef = genericInfo->AST;
    }
  }

  if (!classDef) {
    return nullptr;  // Class AST not found
  }

  // Find the generic method
  for (const auto& methodDecl : classDef->getMethods()) {
    if (methodDecl.function->getProto().getName() == methodName &&
        methodDecl.function->getProto().isGeneric()) {
      genericMethodAST = methodDecl.function.get();
      break;
    }
  }

  if (!genericMethodAST) {
    return nullptr;  // Generic method not found
  }

  const PrototypeAST& proto = genericMethodAST->getProto();
  const auto& methodTypeParams = proto.getTypeParameters();

  if (methodTypeArgs.size() != methodTypeParams.size()) {
    logAndThrowError("Type argument count mismatch for generic method " +
                     methodName);
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
  enterScope();
  addTypeParameterBindings(allTypeParams, allTypeArgs);

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

  // Handle variadic constraint: _init_args<T> -> look up T's init signature
  if (proto.hasVariadicConstraint()) {
    const auto& constraint = *proto.getVariadicConstraint();
    // Check if it's _init_args<T> (may be module-qualified as sun__init_args)
    bool isInitArgs =
        (constraint.baseName == "_init_args" ||
         constraint.baseName.find("_init_args") != std::string::npos);
    if (isInitArgs && !constraint.typeArguments.empty()) {
      // Get the type argument and substitute any type parameters
      sun::TypePtr constraintType =
          typeAnnotationToType(*constraint.typeArguments[0]);
      constraintType = substituteTypeParameters(constraintType);

      // Look up the init signature for this type
      if (constraintType && constraintType->isClass()) {
        auto* targetClass = static_cast<sun::ClassType*>(constraintType.get());
        // Find the init method
        auto initInfo = targetClass->getMethod("init");
        if (initInfo) {
          // ClassMethod::paramTypes excludes 'this', so copy all params
          clonedProto.setResolvedVariadicTypes(initInfo->paramTypes);
        }
      }
    }
  }

  // Clear any stale resolved types from previous specializations
  clearResolvedTypes(*clonedFunc);

  // Lazy parse if body is empty but has source text (from precompiled .moon)
  if (clonedFunc->hasSourceText() && clonedFunc->getBody().getBody().empty()) {
    auto parsedFunc =
        Parser::lazyParseFunctionSource(clonedFunc->getSourceText());
    if (!parsedFunc) {
      exitScope();
      logAndThrowError("Failed to parse lazy method body for: " +
                       genericMethodAST->getProto().getName());
    }
    // Transfer the parsed body to the cloned FunctionAST
    clonedFunc->setBody(std::make_unique<BlockExprAST>(
        std::move(const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
            parsedFunc->getBody().getBody()))));
  }

  // Analyze the method body
  auto savedClass = currentClass;
  setCurrentClass(classType);

  // Extract module prefix from class name (e.g., "sun_Vec" -> "sun")
  // This ensures types used in method bodies resolve correctly
  std::string modulePrefix;
  const std::string& className = classType->getName();
  size_t underscorePos = className.find('_');
  if (underscorePos != std::string::npos) {
    modulePrefix = className.substr(0, underscorePos);
  }

  // Enter module scope if class is from a namespace (for correct type
  // resolution)
  bool inModuleScope = !modulePrefix.empty();
  if (inModuleScope) {
    enterModuleScope(modulePrefix);
    // Add implicit using import for the module so unqualified names resolve
    addUsingImport(UsingImport(modulePrefix, "*"));
  }

  // Compute method signature for nested function qualification
  std::string methodSig = getFunctionSignature(mangledName, paramTypes);

  // Enter method scope and declare parameters
  enterFunctionScope(methodSig);

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
  if (inModuleScope) {
    exitScope();  // module scope
  }
  exitScope();  // type parameter scope

  // Convert to shared_ptr for storage
  std::shared_ptr<FunctionAST> specializedAST = std::move(clonedFunc);

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
