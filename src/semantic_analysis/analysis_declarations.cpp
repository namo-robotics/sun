#include "semantic_analysis/method_signature_set.h"
// analysis_declarations.cpp — Declarations: classes, interfaces, functions,
// modules and type aliases
//
// One handler per AST node kind, called from the dispatcher in
// analysis.cpp.

#include <set>

#include "semantic_analysis/field_initialization.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "support/error.h"

using sun::semantic_analysis::QualifiedName;
using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::PrototypeAST;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::types::LambdaType;

void SemanticAnalyzer::analyzeClassDefinition(
    sun::ast::ClassDefinitionAST& classDef) {
  const std::string& baseName = classDef.getName();

  // Partial classes: add methods to the primary class.
  if (classDef.isPartial()) {
    analyzePartialClass(classDef, classDef);
    return;
  }

  const QualifiedName& qualifiedClass = classDef.getQualifiedName();

  // Forbid redefinition of a class in the same scope
  if (ctx_.declarations().isDeclared(baseName, ctx_.scope())) {
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

  // Validate field names and reject duplicates. Names alone are compared, so
  // this runs before the generic early-return below: a template's field types
  // have no meaning until a specialization substitutes them, but a repeated
  // name is wrong at the declaration either way.
  std::set<std::string> seenFields;
  for (const auto& field : classDef.getFields()) {
    validateNotReserved(field.name, "Field name", field.location);
    if (!seenFields.insert(field.name).second) {
      logAndThrowError("Field '" + field.name + "' already exists in class '" +
                           baseName + "'",
                       field.location);
    }
  }

  // Validate method names
  for (const auto& methodDecl : classDef.getMethods()) {
    const std::string& methodName = methodDecl.function->getProto().getName();
    validateNotReserved(methodName, "Method name",
                        methodDecl.function->getLocation());
  }

  // Lifetime declarations must be distinct, and every lifetime a field
  // names must be the builtin 'this or declared on the class
  for (const auto& lp : classDef.getLifetimeParameters()) {
    if (std::count_if(classDef.getLifetimeParameters().begin(),
                      classDef.getLifetimeParameters().end(),
                      [&](const sun::ast::LifetimeParameter& other) {
                        return other.name == lp.name;
                      }) > 1) {
      logAndThrowError("duplicate lifetime parameter '" + lp.name +
                           " on class '" + baseName + "'",
                       lp.span);
    }
  }
  {
    size_t mark = activeLifetimeNames_.size();
    for (const auto& lp : classDef.getLifetimeParameters()) {
      activeLifetimeNames_.push_back(lp.name);
    }
    bool savedAllowThis = allowThisLifetime_;
    allowThisLifetime_ = true;
    for (const auto& field : classDef.getFields()) {
      checkAnnotationLifetimes(field.type, field.location);
    }
    allowThisLifetime_ = savedAllowThis;
    activeLifetimeNames_.resize(mark);
  }

  // Register in generic class table if this is a generic class or has
  // generic methods (needed for instantiateGenericMethod to find the def)
  if (classDef.isGeneric() || classDef.hasGenericMethods()) {
    GenericClassInfo genericInfo;
    genericInfo.AST = &classDef;
    genericInfo.typeParameters = classDef.getTypeParameters();
    genericInfo.definitionScope = ctx_.scope()->shared_from_this();
    genericInfo.qualifiedName = qualifiedClass;
    ctx_.currentScope().declareGenericClass(baseName, genericInfo);

    // Generic class templates are not analyzed further until instantiated
    if (classDef.isGeneric()) {
      classDef.setResolvedType(Types::Void());
      return;
    }
  }

  // Create the class type with the qualified name
  auto classType =
      ctx_.types()->getClass(classDef.getDeclarationId(), qualifiedClass);

  // Layout must be decided before any getStructType() call memoizes it
  classType->setPacked(classDef.isPacked());
  classType->visibility = classDef.getVisibility();
  {
    std::vector<std::string> lifetimeNames;
    for (const auto& lp : classDef.getLifetimeParameters()) {
      lifetimeNames.push_back(lp.name);
    }
    classType->setLifetimeParams(std::move(lifetimeNames));
  }

  // Register the class BEFORE processing fields to allow self-referential
  // types (e.g., var next: raw_ptr<Node> inside class Node)
  ctx_.currentScope().declareClass(baseName, classType);

  // Fields and method signatures are normally registered by the
  // declaration pre-pass (registerClassShape); classes analyzed outside a
  // pre-passed block register them here.
  bool shapeRegistered =
      ctx_.declarations().hasClassShape(classDef.getDeclarationId());
  if (!shapeRegistered) {
    pipeline_.declarations().registerClassShape(classDef, qualifiedClass,
                                                classType);
  }

  // Inherit interface fields BEFORE analyzing methods
  // This adds interface fields to the class, which methods may access
  inheritInterfaceFields(classDef, classType);

  // Merge methods from any pending class extensions
  // Extensions are collected during import processing and merged here
  // so all methods (primary + extensions) can call each other
  const auto* extensions = ctx_.declarations().pendingExtensions(baseName);
  if (extensions) {
    for (sun::ast::ClassDefinitionAST* extDef : *extensions) {
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
    ctx_.declarations().clearPendingExtensions(baseName);
  }

  // Save old class context and set new one
  auto savedClass = ctx_.getCurrentClass();
  ctx_.setCurrentClass(classType);

  // Enter a Class scope to contain all method scopes in the tree
  ctx_.enterClassScope(qualifiedClass);

  // The class's lifetime names are usable in every member signature
  size_t classLifetimeMark = activeLifetimeNames_.size();
  for (const auto& lp : classDef.getLifetimeParameters()) {
    activeLifetimeNames_.push_back(lp.name);
  }

  // PASS 1: Make the (already registered) method signatures resolvable
  // by source name inside the class scope
  for (const auto& methodDecl : classDef.getMethods()) {
    const PrototypeAST& proto = methodDecl.function->getProto();
    std::string methodNameForScope = proto.getName();
    std::vector<TypePtr> methodParamTypes;
    methodParamTypes.push_back(classType);  // this parameter
    for (const auto& pt : proto.getResolvedParamTypes()) {
      methodParamTypes.push_back(pt);
    }
    TypePtr returnType = proto.hasResolvedReturnType()
                             ? proto.getResolvedReturnType()
                             : Types::Void();
    FunctionInfo methodInfo{returnType, methodParamTypes, {}};
    methodInfo.declarationId = proto.getDeclarationId();
    ctx_.currentScope().declareFunction(methodNameForScope, methodInfo,
                                        ctx_.currentLocation());
  }

  // PASS 2: Analyze all method bodies using their assigned names.
  for (const auto& methodDecl : classDef.getMethods()) {
    bodies_.analyzeFunction(*methodDecl.function);
  }

  // PASS 3: check constructors, now that every method body is analyzed — the
  // walk follows constructor calls into helper bodies, and what it finds
  // there (a bound method reference, say) is only marked once the helper has
  // been analyzed. A precompiled class carries signatures without bodies; its
  // constructors were checked when the bundle was built.
  if (!classDef.isPrecompiled()) {
    for (const auto& methodDecl : classDef.getMethods()) {
      if (!methodDecl.isConstructor) continue;
      sun::semantic_analysis::checkFieldInitialization(
          *methodDecl.function, *classType, classDef.getMethods());
    }
  }

  // Validate interface implementations
  validateInterfaceImplementation(classDef, classType);

  activeLifetimeNames_.resize(classLifetimeMark);
  ctx_.exitScope();  // Class scope

  // Restore old class context
  ctx_.setCurrentClass(savedClass);

  // Track symbol for redefinition detection
  ctx_.declarations().noteDeclared(baseName, ctx_.scope());

  // Store primary AST for partial class merging (if a partial appears
  // later)
  ctx_.currentScope().declareClassDefinition(baseName, classDef);

  // Set resolved type to the class type so codegen can get the qualified
  // name
  classDef.setResolvedType(classType);
}

void SemanticAnalyzer::analyzeInterfaceDefinition(
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
  std::set<std::string> seenInterfaceFields;
  for (const auto& field : interfaceDef.getFields()) {
    validateNotReserved(field.name, "Interface field name", field.location);
    if (!seenInterfaceFields.insert(field.name).second) {
      logAndThrowError("Field '" + field.name +
                           "' already exists in interface '" +
                           interfaceDef.getName() + "'",
                       field.location);
    }
  }

  // Validate method names
  for (const auto& methodDecl : interfaceDef.getMethods()) {
    const std::string& methodName = methodDecl.function->getProto().getName();
    validateNotReserved(methodName, "Interface method name",
                        methodDecl.function->getLocation());
  }

  // Lifetime declarations must be distinct, and every lifetime a member
  // names must be the builtin 'this or declared on the interface
  for (const auto& lp : interfaceDef.getLifetimeParameters()) {
    if (std::count_if(interfaceDef.getLifetimeParameters().begin(),
                      interfaceDef.getLifetimeParameters().end(),
                      [&](const sun::ast::LifetimeParameter& other) {
                        return other.name == lp.name;
                      }) > 1) {
      logAndThrowError("duplicate lifetime parameter '" + lp.name +
                           " on interface '" + interfaceDef.getName() + "'",
                       lp.span);
    }
  }
  size_t interfaceLifetimeMark = activeLifetimeNames_.size();
  for (const auto& lp : interfaceDef.getLifetimeParameters()) {
    activeLifetimeNames_.push_back(lp.name);
  }
  bool savedAllowThisForInterface = allowThisLifetime_;
  allowThisLifetime_ = true;
  for (const auto& field : interfaceDef.getFields()) {
    checkAnnotationLifetimes(field.type, field.location);
  }
  for (const auto& methodDecl : interfaceDef.getMethods()) {
    checkSignatureLifetimes(methodDecl.function->getProto(),
                            methodDecl.function->getLocation());
  }
  allowThisLifetime_ = savedAllowThisForInterface;

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

    interfaceDef.setResolvedType(Types::Void());
    activeLifetimeNames_.resize(interfaceLifetimeMark);
    return;
  }

  // Non-generic interface: create the interface type directly
  auto interfaceType = ctx_.types()->getInterface(
      interfaceDef.getDeclarationId(), qualifiedInterface);
  {
    std::vector<std::string> lifetimeNames;
    for (const auto& lp : interfaceDef.getLifetimeParameters()) {
      lifetimeNames.push_back(lp.name);
    }
    interfaceType->setLifetimeParams(std::move(lifetimeNames));
  }
  // Store the user-written base name for error messages
  if (interfaceName != interfaceDef.getName()) {
    interfaceType->setBaseName(interfaceDef.getName());
  }
  interfaceType->visibility = interfaceDef.getVisibility();
  interfaceType->setQualifiedName(qualifiedInterface);

  // Create a pseudo-class type for 'this' during interface method analysis
  // This allows default implementations to access interface fields
  auto pseudoId = ctx_.results().declarations.add(
      sun::semantic_analysis::DeclarationKind::Class,
      "__interface_" + interfaceDef.getName(), interfaceDef.getDeclarationId(),
      ctx_.results().declarations.get(interfaceDef.getDeclarationId()).module,
      {}, interfaceDef.getDeclarationId(), "interface-receiver");
  auto pseudoClass = ctx_.types()->getClass(pseudoId);

  // Add fields to the interface type and pseudo-class
  for (const auto& field : interfaceDef.getFields()) {
    TypePtr fieldType = resolver_.typeAnnotationToType(field.type);
    interfaceType->addField(field.name, fieldType, field.declaration.id)
        .visibility = field.visibility;
    pseudoClass->addField(field.name, fieldType, field.declaration.id)
        .visibility = field.visibility;
  }

  // Add methods to the interface type, rejecting duplicate signatures.
  MethodSignatureSet methodSignatures(ctx_, resolver_);
  for (const auto& methodDecl : interfaceDef.getMethods()) {
    // Get method signature info (pure computation)
    FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
    PrototypeAST& proto =
        const_cast<PrototypeAST&>(methodDecl.function->getProto());

    // Apply computed info to prototype
    applyFunctionInfoToProto(proto, methodInfo);

    if (!methodSignatures.insert(proto, methodInfo.paramTypes))
      logAndThrowError(
          "Interface method '" + proto.getName() + "' is already defined",
          methodDecl.function->getLocation());
    // Add method to interface type (include generic type parameters)
    auto& method = interfaceType->addMethod(
        proto.getName(), methodInfo.returnType, methodInfo.paramTypes,
        methodDecl.hasDefaultImpl, proto.getTypeParameterNames());
    method.declarationId = proto.getDeclarationId();
    method.visibility =
        sun::semantic_analysis::methodVisibility(*methodDecl.function);
    method.isConst = methodDecl.isConst;
    method.isUnsafe = methodDecl.function->getProto().isUnsafeMethod();
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
      bodies_.analyzeFunction(*methodDecl.function);

      // Restore original ctx_.getCurrentClass()
      ctx_.setCurrentClass(savedClass);
    }
  }

  ctx_.exitScope();  // Interface scope

  // Register the interface
  ctx_.currentScope().declareInterface(interfaceDef.getName(), interfaceType);

  // Track symbol for redefinition detection
  ctx_.declarations().noteDeclared(interfaceDef.getName(), ctx_.scope());

  activeLifetimeNames_.resize(interfaceLifetimeMark);
  interfaceDef.setResolvedType(Types::Void());
}

void SemanticAnalyzer::analyzeFunctionDefinition(sun::ast::FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Get function signature info (includes qualified name with function
  // context)
  FunctionInfo funcInfo = getFunctionInfo(func);

  if (funcInfo.isForwardDeclaration) {
    const sun::semantic_analysis::CallableSignature signature{
        funcInfo.qualifiedName.baseName, funcInfo.paramTypes};
    const auto definition = ctx_.scope()->functions.find(signature);
    if (definition != ctx_.scope()->functions.end() &&
        !definition->second.isForwardDeclaration) {
      if (!definition->second.returnType->equals(*funcInfo.returnType) ||
          definition->second.canThrow != funcInfo.canThrow)
        logAndThrowError("Forward declaration does not match its definition",
                         func.getLocation());
      func.setTargetDeclarationId(definition->second.declarationId);
    }
  }

  // Apply computed info to prototype
  applyFunctionInfoToProto(proto, funcInfo);

  // Templates were registered during declaration collection.
  // Only register non-template functions in the normal function table.
  // Templates are looked up via the genericFunctions table instead.
  if (!proto.isTemplate()) {
    ctx_.currentScope().declareFunction(funcInfo.qualifiedName.baseName,
                                        funcInfo, ctx_.currentLocation());
  }

  // Analyze the function body
  bodies_.analyzeFunction(func);

  // Set the function type on the function node
  func.setResolvedType(Types::Function(funcInfo.returnType, funcInfo.paramTypes,
                                       funcInfo.canThrow));
}

void SemanticAnalyzer::analyzeLambdaExpr(sun::ast::LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  rejectRefEnvReturnType(proto.getReturnType(), lambda.getLocation(),
                         /*allowNamed=*/true);

  // A lambda's own binders and any enclosing binders are both in scope for
  // its signature.
  checkSignatureLifetimes(proto, lambda.getLocation());

  // Get lambda signature info (pure computation)
  FunctionInfo lambdaInfo = getLambdaInfo(lambda);

  // Apply computed info to prototype
  applyFunctionInfoToProto(proto, lambdaInfo);

  // Analyze the lambda body
  bodies_.analyzeLambda(lambda);

  // Set the lambda type on the lambda node
  auto lambdaType =
      Types::Lambda(lambdaInfo.returnType, lambdaInfo.paramTypes,
                    proto.hasReturnType() && proto.getReturnType()->canError);
  if (proto.hasRefCaptures() || !proto.getRefCaptureNames().empty() ||
      !proto.getOwnedCaptureNames().empty())
    static_cast<LambdaType&>(*lambdaType).setHasRefCaptures(true);
  lambda.setResolvedType(lambdaType);
}

void SemanticAnalyzer::analyzeModuleDefinition(sun::ast::ModuleAST& nsDecl) {
  // Enter the namespace scope
  ctx_.enterScope(ctx_.currentScope().declareModule(nsDecl));

  // Analyze the body of the namespace
  // Functions handle their own qualified name registration in FUNCTION case
  for (const auto& bodyExpr : nsDecl.getBody().getBody()) {
    if (bodyExpr->getType() == sun::ast::ASTNodeType::VARIABLE_CREATION) {
      // Variables need special handling to register in namespacedVariables
      auto& varCreate = static_cast<sun::ast::VariableCreationAST&>(*bodyExpr);
      if (bodyExpr->isPrecompiled()) {
        // A global imported from a .moon: the storage and its initial
        // value live in the bundle, so there is nothing to analyze — only
        // the type to resolve and the name to register.
        pipeline_.declarations().registerPrecompiledModuleVariable(varCreate);
        continue;
      }
      // A tracked global registers itself, possibly before the walk gets
      // here; any other variable is registered now.
      const bool tracked = isTrackedGlobal(varCreate);
      analyzeExpr(*bodyExpr);
      if (!tracked) registerModuleVariable(varCreate);
    } else {
      analyzeExpr(*bodyExpr);
    }
  }

  // Exit the namespace scope
  ctx_.exitScope();
  nsDecl.setResolvedType(Types::Void());
}

void SemanticAnalyzer::analyzeMoonScope(sun::ast::ExprAST& expr) {
  // A bundle's declarations live under its content hash, whether they are
  // an import's stubs or the sources of the bundle being built
  auto& moonScope = static_cast<sun::ast::MoonScopeAST&>(expr);
  const std::string& contentHash = moonScope.getContentHash();
  if (!contentHash.empty()) {
    ctx_.enterModuleScope(contentHash);
  }
  // An import's contents are stubs — signatures whose bodies were stripped
  // when the moon was built — so body-shape checks are off while we are in
  // there. The bundle being built is ordinary source and keeps every check.
  const bool stubs = !moonScope.isOwnBundle();
  if (stubs) ctx_.enterMoonScope();
  for (const auto& bodyExpr : moonScope.getBody().getBody()) {
    analyzeExpr(*bodyExpr);
  }
  if (stubs) ctx_.exitMoonScope();
  if (!contentHash.empty()) {
    ctx_.exitScope();
  }
  expr.setResolvedType(Types::Void());
}

void SemanticAnalyzer::analyzeDeclareType(
    sun::ast::DeclareTypeAST& declareExpr) {
  // Trigger generic instantiation by resolving the type annotation
  TypePtr resolvedType =
      resolver_.typeAnnotationToType(declareExpr.getTypeAnnotation());
  declareExpr.setResolvedDeclaredType(resolvedType);

  // If there's an alias, register it
  if (declareExpr.hasAlias()) {
    const std::string& aliasName = declareExpr.getAliasName();
    ctx_.currentScope().declareTypeAlias(aliasName, resolvedType,
                                         declareExpr.getLocation());
  }

  declareExpr.setResolvedType(Types::Void());
}

}  // namespace sun::semantic_analysis
