// analysis_declarations.cpp — Declarations: classes, interfaces, functions,
// modules and type aliases
//
// One handler per AST node kind, called from the dispatcher in
// analysis.cpp.

#include "semantic_analysis/field_initialization.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

void SemanticAnalyzer::analyzeClassDefinition(ClassDefinitionAST& classDef) {
  const std::string& baseName = classDef.getName();

  // Partial classes: add methods to the primary class.
  if (classDef.isPartial()) {
    analyzePartialClass(classDef, classDef);
    return;
  }

  // Qualify class name with module prefix if inside a module
  // For precompiled classes (from .moon), use the qualified name from
  // metadata (includes content hash prefix for symbol isolation)
  sun::QualifiedName qualifiedClass;
  if (classDef.hasQualifiedName()) {
    qualifiedClass = classDef.getQualifiedName();
  } else {
    qualifiedClass = ctx_.makeQualifiedName(baseName);
    classDef.setQualifiedName(qualifiedClass);
  }
  std::string mangledClassName = qualifiedClass.mangled();

  // Forbid redefinition of class in same module
  if (declarations_.isDeclared(mangledClassName)) {
    logAndThrowError("Redefinition of class '" + baseName + "'",
                     classDef.getLocation());
  }

  // Validate class name
  validateNotReserved(classDef.getName(), "Class name", classDef.getLocation());

  // Check for redefinition of builtin types
  if (ctx_.types()->isBuiltinTypeName(classDef.getName())) {
    logAndThrowError(
        "Cannot redefine builtin type '" + classDef.getName() + "'",
        classDef.getLocation());
  }

  // Validate field names
  for (const auto& field : classDef.getFields()) {
    validateNotReserved(field.name, "Field name", field.location);
  }

  // Validate method names
  for (const auto& methodDecl : classDef.getMethods()) {
    const std::string& methodName = methodDecl.function->getProto().getName();
    validateNotReserved(methodName, "Method name",
                        methodDecl.function->getLocation());
  }

  // Register in generic class table if this is a generic class or has
  // generic methods (needed for instantiateGenericMethod to find the def)
  if (classDef.isGeneric() || classDef.hasGenericMethods()) {
    GenericClassInfo genericInfo;
    genericInfo.AST = &classDef;
    genericInfo.typeParameters = classDef.getTypeParameters();
    genericInfo.definitionScope = ctx_.scope()->shared_from_this();
    genericInfo.qualifiedName = qualifiedClass;
    ctx_.registerGenericClass(baseName, genericInfo);

    // Generic class templates are not analyzed further until instantiated
    if (classDef.isGeneric()) {
      classDef.setResolvedType(sun::Types::Void());
      return;
    }
  }

  // Create the class type with the qualified name
  auto classType = ctx_.types()->getClass(qualifiedClass);

  // Layout must be decided before any getStructType() call memoizes it
  classType->setPacked(classDef.isPacked());
  classType->visibility = classDef.getVisibility();

  // Register the class BEFORE processing fields to allow self-referential
  // types (e.g., var next: raw_ptr<Node> inside class Node)
  ctx_.registerClass(baseName, classType);

  // Fields and method signatures are normally registered by the
  // declaration pre-pass (registerClassShape); classes analyzed outside a
  // pre-passed block register them here.
  bool shapeRegistered = declarations_.hasClassShape(mangledClassName);
  if (!shapeRegistered) {
    declarations_.registerClassShape(classDef, qualifiedClass, classType);
  }

  // Inherit interface fields BEFORE analyzing methods
  // This adds interface fields to the class, which methods may access
  inheritInterfaceFields(classDef, classType);

  // Merge methods from any pending class extensions
  // Extensions are collected during import processing and merged here
  // so all methods (primary + extensions) can call each other
  const auto* extensions = declarations_.pendingExtensions(baseName);
  if (extensions) {
    for (ClassDefinitionAST* extDef : *extensions) {
      // Validate: check for duplicate methods
      for (const auto& extMethod : extDef->getMethods()) {
        const std::string& methodName =
            extMethod.function->getProto().getName();
        // Check against primary's methods
        for (const auto& primaryMethod : classDef.getMethods()) {
          if (primaryMethod.function->getProto().getName() == methodName) {
            logAndThrowError("Method '" + methodName +
                                 "' already defined in class '" + baseName +
                                 "'",
                             extMethod.function->getLocation());
          }
        }
        // Check against other extension methods already merged
        // (The getMutableMethods approach handles this via sequential
        // merge)
      }
      // Merge extension methods into the primary class definition
      for (auto& extMethod : extDef->getMutableMethods()) {
        classDef.getMutableMethods().push_back(std::move(extMethod));
      }
    }
    // Clear the pending extensions for this class (they're now merged)
    declarations_.clearPendingExtensions(baseName);
  }

  // Save old class context and set new one
  auto savedClass = ctx_.getCurrentClass();
  ctx_.setCurrentClass(classType);

  // Enter a Class scope to contain all method scopes in the tree
  ctx_.enterClassScope(qualifiedClass);

  // PASS 1: Make the (already registered) method signatures resolvable
  // by mangled name inside the class scope
  for (const auto& methodDecl : classDef.getMethods()) {
    const PrototypeAST& proto = methodDecl.function->getProto();
    std::string mangledName = classType->getMangledMethodName(proto.getName());
    std::vector<sun::TypePtr> methodParamTypes;
    methodParamTypes.push_back(classType);  // this parameter
    for (const auto& pt : proto.getResolvedParamTypes()) {
      methodParamTypes.push_back(pt);
    }
    sun::TypePtr returnType = proto.hasResolvedReturnType()
                                  ? proto.getResolvedReturnType()
                                  : sun::Types::Void();
    ctx_.registerFunctionInCurrentScope(mangledName,
                                        {returnType, methodParamTypes, {}});
  }

  // PASS 2: Analyze all method bodies
  for (size_t i = 0; i < classDef.getMethods().size(); ++i) {
    const auto& methodDecl = classDef.getMethods()[i];
    // Set qualified name: scopePath includes module and class context
    PrototypeAST& proto =
        const_cast<PrototypeAST&>(methodDecl.function->getProto());
    std::vector<std::string> methodScopePath = qualifiedClass.scopePath;
    methodScopePath.push_back(mangledClassName);
    proto.setQualifiedName(
        sun::QualifiedName(methodScopePath, proto.getName()));
    analyzeFunction(*methodDecl.function);
  }

  // PASS 3: check constructors, now that every method body is analyzed — the
  // walk follows constructor calls into helper bodies, and what it finds
  // there (a bound method reference, say) is only marked once the helper has
  // been analyzed. A precompiled class carries signatures without bodies; its
  // constructors were checked when the bundle was built.
  if (!classDef.isPrecompiled()) {
    for (const auto& methodDecl : classDef.getMethods()) {
      if (!methodDecl.isConstructor) continue;
      sun::checkFieldInitialization(*methodDecl.function, *classType,
                                    classDef.getMethods());
    }
  }

  // Validate interface implementations
  validateInterfaceImplementation(classDef, classType);

  ctx_.exitScope();  // Class scope

  // Restore old class context
  ctx_.setCurrentClass(savedClass);

  // Track symbol for redefinition detection
  declarations_.noteDeclared(mangledClassName);

  // Store primary AST for partial class merging (if a partial appears
  // later)
  ctx_.scope()->classDefinitions[baseName] = &classDef;

  // Set resolved type to the class type so codegen can get the qualified
  // name
  classDef.setResolvedType(classType);
}

void SemanticAnalyzer::analyzeInterfaceDefinition(
    InterfaceDefinitionAST& interfaceDef) {
  // Qualify interface name with module prefix if inside a module
  // For precompiled interfaces (from .moon), use the qualified name from
  // metadata
  sun::QualifiedName qualifiedInterface;
  if (interfaceDef.hasQualifiedName()) {
    qualifiedInterface = interfaceDef.getQualifiedName();
  } else {
    qualifiedInterface = ctx_.makeQualifiedName(interfaceDef.getName());
    interfaceDef.setQualifiedName(qualifiedInterface);
  }
  std::string interfaceName = qualifiedInterface.mangled();

  // Forbid redefinition of interface in same module
  if (declarations_.isDeclared(interfaceName)) {
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

  // Validate field names
  for (const auto& field : interfaceDef.getFields()) {
    validateNotReserved(field.name, "Interface field name", field.location);
  }

  // Validate method names
  for (const auto& methodDecl : interfaceDef.getMethods()) {
    const std::string& methodName = methodDecl.function->getProto().getName();
    validateNotReserved(methodName, "Interface method name",
                        methodDecl.function->getLocation());
  }

  // Handle generic interfaces differently
  if (interfaceDef.isGeneric()) {
    // Register as generic interface template for later instantiation
    GenericInterfaceInfo info;
    info.AST = &interfaceDef;
    info.typeParameters = interfaceDef.getTypeParameters();
    info.qualifiedName = qualifiedInterface;
    ctx_.registerGenericInterface(interfaceDef.getName(), info);

    // Create a generic interface type (for type checking generic
    // references)
    auto interfaceType = ctx_.types()->getGenericInterface(
        interfaceDef.getName(), interfaceDef.getTypeParameterNames());
    interfaceType->visibility = interfaceDef.getVisibility();
    interfaceType->setQualifiedName(qualifiedInterface);
    ctx_.registerInterface(interfaceDef.getName(), interfaceType);

    interfaceDef.setResolvedType(sun::Types::Void());
    return;
  }

  // Non-generic interface: create the interface type directly
  auto interfaceType = ctx_.types()->getInterface(interfaceName);
  // Store the user-written base name for error messages
  if (interfaceName != interfaceDef.getName()) {
    interfaceType->setBaseName(interfaceDef.getName());
  }
  interfaceType->visibility = interfaceDef.getVisibility();
  interfaceType->setQualifiedName(qualifiedInterface);

  // Create a pseudo-class type for 'this' during interface method analysis
  // This allows default implementations to access interface fields
  auto pseudoClass =
      ctx_.types()->getClass("__interface_" + interfaceDef.getName());

  // Add fields to the interface type and pseudo-class
  for (const auto& field : interfaceDef.getFields()) {
    sun::TypePtr fieldType = types_.typeAnnotationToType(field.type);
    interfaceType->addField(field.name, fieldType).visibility =
        field.visibility;
    pseudoClass->addField(field.name, fieldType).visibility = field.visibility;
  }

  // Add methods to the interface type
  for (const auto& methodDecl : interfaceDef.getMethods()) {
    // Get method signature info (pure computation)
    FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
    PrototypeAST& proto =
        const_cast<PrototypeAST&>(methodDecl.function->getProto());

    // Apply computed info to prototype
    applyFunctionInfoToProto(proto, methodInfo);

    // Add method to interface type (include generic type parameters)
    auto& method = interfaceType->addMethod(
        proto.getName(), methodInfo.returnType, methodInfo.paramTypes,
        methodDecl.hasDefaultImpl, proto.getTypeParameterNames());
    method.visibility = sun::access::methodVisibility(*methodDecl.function);
    method.isConst = methodDecl.isConst;
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
      analyzeFunction(*methodDecl.function);

      // Restore original ctx_.getCurrentClass()
      ctx_.setCurrentClass(savedClass);
    }
  }

  ctx_.exitScope();  // Interface scope

  // Register the interface
  ctx_.registerInterface(interfaceDef.getName(), interfaceType);

  // Track symbol for redefinition detection
  declarations_.noteDeclared(interfaceName);

  interfaceDef.setResolvedType(sun::Types::Void());
}

void SemanticAnalyzer::analyzeFunctionDefinition(FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Get function signature info (includes qualified name with function
  // context)
  FunctionInfo funcInfo = getFunctionInfo(func);

  // Apply computed info to prototype
  applyFunctionInfoToProto(proto, funcInfo);
  proto.setQualifiedName(funcInfo.qualifiedName);

  // Register templates for later instantiation. A pack makes a function a
  // template even with no type parameters: its arity comes from the call.
  if (proto.isTemplate() && !ctx_.getCurrentClass()) {
    ctx_.registerGenericFunctionInCurrentScope(func);
  }

  // Only register non-template functions in the normal function table.
  // Templates are looked up via the genericFunctions table instead.
  if (!proto.isTemplate()) {
    ctx_.registerFunctionInCurrentScope(funcInfo.qualifiedName.baseName,
                                        funcInfo);
  }

  // Analyze the function body
  analyzeFunction(func);

  // Set the function type on the function node
  func.setResolvedType(types_.inferType(func));
}

void SemanticAnalyzer::analyzeLambdaExpr(LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Get lambda signature info (pure computation)
  FunctionInfo lambdaInfo = getLambdaInfo(lambda);

  // Apply computed info to prototype
  applyFunctionInfoToProto(proto, lambdaInfo);

  // Analyze the lambda body
  analyzeLambda(lambda);

  // Set the lambda type on the lambda node
  lambda.setResolvedType(types_.inferType(lambda));
}

void SemanticAnalyzer::analyzeModuleDefinition(ModuleAST& nsDecl) {
  // Enter the namespace scope
  ctx_.declareModule(nsDecl);

  // Analyze the body of the namespace
  // Functions handle their own qualified name registration in FUNCTION case
  for (const auto& bodyExpr : nsDecl.getBody().getBody()) {
    if (bodyExpr->getType() == ASTNodeType::VARIABLE_CREATION) {
      // Variables need special handling to register in namespacedVariables
      auto& varCreate = static_cast<VariableCreationAST&>(*bodyExpr);
      if (bodyExpr->isPrecompiled()) {
        // A global imported from a .moon: the storage and its initial
        // value live in the bundle, so there is nothing to analyze — only
        // the type to resolve and the name to register.
        declarations_.registerPrecompiledModuleVariable(varCreate);
        continue;
      }
      analyzeExpr(*bodyExpr);
      sun::QualifiedName qualifiedName =
          ctx_.makeQualifiedName(varCreate.getName());
      varCreate.setQualifiedName(qualifiedName);
      if (auto type = varCreate.getResolvedType()) {
        ctx_.registerModuleVariable(
            varCreate.getName(), qualifiedName.mangled(), type,
            varCreate.getVisibility(), varCreate.isConst());
      }
    } else if (bodyExpr->getType() == ASTNodeType::REFERENCE_CREATION) {
      analyzeExpr(*bodyExpr);
      auto& refCreate = static_cast<ReferenceCreationAST&>(*bodyExpr);
      sun::QualifiedName qualifiedName =
          ctx_.makeQualifiedName(refCreate.getName());
      refCreate.setQualifiedName(qualifiedName);
    } else {
      analyzeExpr(*bodyExpr);
    }
  }

  // Exit the namespace scope
  ctx_.exitScope();
  nsDecl.setResolvedType(sun::Types::Void());
}

void SemanticAnalyzer::analyzeMoonScope(ExprAST& expr) {
  // MoonScopeAST wraps module stubs from a moon import
  // Enter a scope with the content hash prefix for symbol isolation
  auto& moonScope = static_cast<MoonScopeAST&>(expr);
  const std::string& contentHash = moonScope.getContentHash();
  if (!contentHash.empty()) {
    ctx_.enterModuleScope(contentHash);
  }
  // Analyze contained ModuleAST nodes
  for (const auto& bodyExpr : moonScope.getBody().getBody()) {
    analyzeExpr(*bodyExpr);
  }
  if (!contentHash.empty()) {
    ctx_.exitScope();
  }
  expr.setResolvedType(sun::Types::Void());
}

void SemanticAnalyzer::analyzeDeclareType(DeclareTypeAST& declareExpr) {
  // Trigger generic instantiation by resolving the type annotation
  sun::TypePtr resolvedType =
      types_.typeAnnotationToType(declareExpr.getTypeAnnotation());
  declareExpr.setResolvedDeclaredType(resolvedType);

  // If there's an alias, register it
  if (declareExpr.hasAlias()) {
    const std::string& aliasName = declareExpr.getAliasName();
    // Check current scope only for redefinition (shadowing is allowed)
    if (ctx_.scope()->typeAliases.find(aliasName) !=
        ctx_.scope()->typeAliases.end()) {
      logAndThrowError(
          "Type alias '" + aliasName + "' is already defined in this scope",
          declareExpr.getLocation());
    }
    if (resolvedType) {
      ctx_.scope()->typeAliases[aliasName] = resolvedType;
    }
  }

  declareExpr.setResolvedType(sun::Types::Void());
}
