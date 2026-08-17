// semantic_analysis/declaration_collection.cpp
//
// Declaration pre-pass: registers all top-level declarations (functions,
// classes, interfaces, enums, modules) before analyzing bodies. This allows
// forward references between declarations at the same scope level.

#include "semantic_analyzer.h"

void SemanticAnalyzer::collectDeclarations(BlockExprAST& block) {
  // Only hoist at module level (not inside function bodies where captures
  // and local variable ordering matter)
  if (!isAtModuleLevel()) return;

  // Nested calls (modules) share the outermost pre-pass; specialization
  // bodies deferred anywhere inside are analyzed when it completes.
  struct PrepassGuard {
    SemanticAnalyzer& a;
    bool outermost;
    explicit PrepassGuard(SemanticAnalyzer& an)
        : a(an), outermost(an.declarationPrepassDepth_ == 0) {
      ++a.declarationPrepassDepth_;
    }
    ~PrepassGuard() {
      --a.declarationPrepassDepth_;
      if (outermost) a.analyzeDeferredSpecializations();
    }
  } prepassGuard(*this);

  // Sub-pass A: Register types (enums, interfaces, classes) so that
  // function signatures can reference forward-declared types.
  for (const auto& expr : block.getBody()) {
    // Precompiled nodes (from .moon libs) are registered here like any
    // other type declaration — registration is idempotent, and every type,
    // generic template and class shape must be known before any signature
    // (precompiled or not) is resolved. Their bodies are never analyzed.

    switch (expr->getType()) {
      case ASTNodeType::ENUM_DEFINITION: {
        auto& enumDef = static_cast<EnumDefinitionAST&>(*expr);
        // Generic enums register as templates, instantiated at use sites
        if (enumDef.isGeneric()) {
          if (!lookupGenericEnum(enumDef.getName())) {
            registerGenericEnum(
                enumDef.getName(),
                {&enumDef, enumDef.getTypeParameters(),
                 makeQualifiedName(enumDef.getName())});
          }
          break;
        }
        // Skip if already registered (e.g. from import)
        if (lookupEnum(enumDef.getName())) break;
        // Create and register a minimal enum type
        auto enumType = typeRegistry->getEnum(enumDef.getName());
        for (const auto& variant : enumDef.getVariants()) {
          enumType->addVariant(variant.name, variant.value);
        }
        enumType->visibility = enumDef.getVisibility();
        enumType->setQualifiedName(makeQualifiedName(enumDef.getName()));
        registerEnum(enumDef.getName(), enumType);
        break;
      }
      case ASTNodeType::INTERFACE_DEFINITION: {
        auto& interfaceDef = static_cast<InterfaceDefinitionAST&>(*expr);
        // Skip if already registered
        if (lookupInterface(interfaceDef.getName())) break;
        if (interfaceDef.isGeneric()) {
          if (!lookupGenericInterface(interfaceDef.getName())) {
            GenericInterfaceInfo info;
            info.AST = &interfaceDef;
            info.typeParameters = interfaceDef.getTypeParameters();
            info.qualifiedName = makeQualifiedName(interfaceDef.getName());
            registerGenericInterface(interfaceDef.getName(), info);
          }
        } else {
          // Precompiled stubs carry their qualified name (content-hash scoped)
          sun::QualifiedName qualifiedInterface =
              interfaceDef.hasQualifiedName()
                  ? interfaceDef.getQualifiedName()
                  : makeQualifiedName(interfaceDef.getName());
          std::string interfaceName = qualifiedInterface.mangled();
          auto interfaceType = typeRegistry->getInterface(interfaceName);
          if (interfaceName != interfaceDef.getName()) {
            interfaceType->setBaseName(interfaceDef.getName());
          }
          interfaceType->visibility = interfaceDef.getVisibility();
          interfaceType->setQualifiedName(qualifiedInterface);
          registerInterface(interfaceDef.getName(), interfaceType);
        }
        break;
      }
      case ASTNodeType::CLASS_DEFINITION: {
        auto& classDef = static_cast<ClassDefinitionAST&>(*expr);
        if (classDef.isPartial()) break;
        // Skip if already registered
        if (lookupClass(classDef.getName())) break;
        // Precompiled stubs carry their qualified name (content-hash scoped)
        sun::QualifiedName qualifiedClass =
            classDef.hasQualifiedName() ? classDef.getQualifiedName()
                                        : makeQualifiedName(classDef.getName());
        if (classDef.isGeneric() || classDef.hasGenericMethods()) {
          GenericClassInfo genericInfo;
          genericInfo.AST = &classDef;
          genericInfo.typeParameters = classDef.getTypeParameters();
          genericInfo.definitionScope = currentScope->shared_from_this();
          genericInfo.qualifiedName = qualifiedClass;
          registerGenericClass(classDef.getName(), genericInfo);
        }
        if (!classDef.isGeneric()) {
          auto classType = typeRegistry->getClass(qualifiedClass);
          classType->setPacked(classDef.isPacked());
          classType->visibility = classDef.getVisibility();
          registerClass(classDef.getName(), classType);
        }
        break;
      }
      case ASTNodeType::USING: {
        // Bind imports in declaration order so nested modules and the shape
        // / signature passes below resolve imported names (moon-imported
        // module scopes precede user code in the body)
        registerUsing(static_cast<UsingAST&>(*expr));
        break;
      }
      case ASTNodeType::MODULE: {
        auto& nsDecl = static_cast<ModuleAST&>(*expr);
        declareModule(nsDecl);
        collectDeclarations(const_cast<BlockExprAST&>(nsDecl.getBody()));
        exitScope();
        break;
      }
      case ASTNodeType::MOON_SCOPE: {
        // Process the contained module stubs with content hash prefix
        auto& moonScope = static_cast<MoonScopeAST&>(*expr);
        const std::string& contentHash = moonScope.getContentHash();
        if (!contentHash.empty()) {
          enterModuleScope(contentHash);
        }
        collectDeclarations(const_cast<BlockExprAST&>(moonScope.getBody()));
        if (!contentHash.empty()) {
          exitScope();
        }
        break;
      }
      default:
        break;
    }
  }

  // Sub-pass A2: Register class shapes (fields + method signatures) for the
  // non-generic classes just registered. Function signatures in sub-pass B
  // may instantiate generic classes, and those specializations' method bodies
  // may call methods of any class in this block — so every class's methods
  // must be known before any signature is resolved.
  for (const auto& expr : block.getBody()) {
    if (expr->getType() != ASTNodeType::CLASS_DEFINITION) continue;
    auto& classDef = static_cast<ClassDefinitionAST&>(*expr);
    if (classDef.isPartial() || classDef.isGeneric()) continue;
    sun::QualifiedName qualifiedClass = classDef.hasQualifiedName()
                                            ? classDef.getQualifiedName()
                                            : makeQualifiedName(classDef.getName());
    if (preRegisteredClassShapes_.count(qualifiedClass.mangled())) continue;
    auto classType = lookupClass(classDef.getName());
    if (!classType) continue;
    registerClassShape(classDef, qualifiedClass, classType);
  }

  // Sub-pass B: Register functions (signatures only, no body analysis).
  // Types are now available for parameter/return type resolution.
  for (const auto& expr : block.getBody()) {
    // Skip precompiled nodes (from .moon libs)
    if (expr->isPrecompiled()) continue;

    switch (expr->getType()) {
      case ASTNodeType::FUNCTION:
        collectFunctionSignature(static_cast<FunctionAST&>(*expr));
        break;
      case ASTNodeType::MODULE: {
        auto& nsDecl = static_cast<ModuleAST&>(*expr);
        enterModuleScope(nsDecl.getName());
        // Register the module's enums first: function signatures below may
        // use enum types (including generic enums like Option<i32>)
        collectEnumDeclarations(nsDecl.getBody());
        // Collect function declarations inside the module
        for (const auto& bodyExpr :
             const_cast<BlockExprAST&>(nsDecl.getBody()).getBody()) {
          if (bodyExpr->getType() == ASTNodeType::FUNCTION)
            collectFunctionSignature(static_cast<FunctionAST&>(*bodyExpr));
        }
        exitScope();
        break;
      }
      case ASTNodeType::MOON_SCOPE: {
        // Process the contained module stubs with content hash prefix
        auto& moonScope = static_cast<MoonScopeAST&>(*expr);
        const std::string& contentHash = moonScope.getContentHash();
        if (!contentHash.empty()) {
          enterModuleScope(contentHash);
        }
        for (const auto& bodyExpr :
             const_cast<BlockExprAST&>(moonScope.getBody()).getBody()) {
          if (bodyExpr->getType() == ASTNodeType::MODULE) {
            auto& nsDecl = static_cast<ModuleAST&>(*bodyExpr);
            enterModuleScope(nsDecl.getName());
            // Register the module's enums first: function signatures below
            // may use enum types (including generic enums like Option<i32>)
            collectEnumDeclarations(nsDecl.getBody());
            // Collect function declarations inside the module
            for (const auto& moduleExpr :
                 const_cast<BlockExprAST&>(nsDecl.getBody()).getBody()) {
              if (moduleExpr->getType() == ASTNodeType::FUNCTION)
                collectFunctionSignature(
                    static_cast<FunctionAST&>(*moduleExpr));
            }
            exitScope();
          }
        }
        if (!contentHash.empty()) {
          exitScope();
        }
        break;
      }
      default:
        break;
    }
  }
}

// Register a named, non-lambda function's signature (no body analysis) in
// the current scope. Generic functions register as templates.
void SemanticAnalyzer::collectFunctionSignature(FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Skip lambdas and anonymous functions
  if (proto.getName().empty()) return;

  if (proto.isGeneric()) {
    registerGenericFunctionInCurrentScope(func);
    return;
  }

  std::vector<sun::TypePtr> paramTypes;
  for (auto& [argName, argType] : proto.getMutableArgs()) {
    paramTypes.push_back(typeAnnotationToType(argType));
  }
  sun::TypePtr returnType = sun::Types::Void();
  if (proto.hasReturnType()) {
    returnType = typeAnnotationToType(*proto.getReturnType());
  }

  // C externs bind to a fixed symbol: no module scope, no overload suffix
  // (see getFunctionInfo).
  sun::QualifiedName qualifiedName =
      func.isCExtern() ? sun::QualifiedName({}, proto.getName())
                       : makeQualifiedName(proto.getName());
  if (!func.isCExtern()) qualifiedName.setParamSuffix(paramTypes);

  // Minimal FunctionInfo (no captures — those require body analysis)
  FunctionInfo info;
  info.returnType = returnType;
  info.paramTypes = std::move(paramTypes);
  info.qualifiedName = qualifiedName;
  info.canThrow = proto.canThrow();
  info.isCVariadic = proto.isCVariadic();
  info.isCExtern = func.isCExtern();
  info.visibility = func.getVisibility();

  registernFunctionInCurrentScope(qualifiedName.baseName, info);
}
