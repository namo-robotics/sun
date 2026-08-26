// declaration_collector.cpp — The declaration pre-pass (see
// declaration_collector.h)

#include "semantic_analysis/declaration_collector.h"

#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/config.h"
#include "support/error.h"

using sun::access::methodVisibility;

void DeclarationCollector::collectDeclarations(BlockExprAST& block) {
  // Only hoist at module level (not inside function bodies where captures
  // and local variable ordering matter)
  if (!ctx_.isAtModuleLevel()) return;

  // Nested calls (modules) share the outermost pre-pass; specialization
  // bodies deferred anywhere inside are analyzed when it completes.
  struct PrepassGuard {
    DeclarationCollector& c;
    GenericSpecializer& generics;
    bool outermost;
    PrepassGuard(DeclarationCollector& collector, GenericSpecializer& g)
        : c(collector), generics(g), outermost(collector.prepassDepth_ == 0) {
      ++c.prepassDepth_;
      generics.setInDeclarationPrepass(true);
    }
    ~PrepassGuard() {
      --c.prepassDepth_;
      if (outermost) {
        generics.setInDeclarationPrepass(false);
        generics.analyzeDeferredSpecializations();
      }
    }
  } prepassGuard(*this, sema_.generics());

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
          if (!ctx_.lookupGenericEnum(enumDef.getName())) {
            ctx_.registerGenericEnum(
                enumDef.getName(), {&enumDef, enumDef.getTypeParameters(),
                                    ctx_.makeQualifiedName(enumDef.getName())});
          }
          break;
        }
        // Skip if already registered (e.g. from import)
        if (ctx_.lookupEnum(enumDef.getName())) break;
        // Create and register a minimal enum type
        auto enumType = ctx_.types()->getEnum(enumDef.getName());
        for (const auto& variant : enumDef.getVariants()) {
          enumType->addVariant(variant.name, variant.value);
        }
        enumType->visibility = enumDef.getVisibility();
        enumType->setQualifiedName(ctx_.makeQualifiedName(enumDef.getName()));
        ctx_.registerEnum(enumDef.getName(), enumType);
        break;
      }
      case ASTNodeType::INTERFACE_DEFINITION: {
        auto& interfaceDef = static_cast<InterfaceDefinitionAST&>(*expr);
        // Skip if already registered
        if (ctx_.lookupInterface(interfaceDef.getName())) break;
        if (interfaceDef.isGeneric()) {
          if (!ctx_.lookupGenericInterface(interfaceDef.getName())) {
            GenericInterfaceInfo info;
            info.AST = &interfaceDef;
            info.typeParameters = interfaceDef.getTypeParameters();
            info.qualifiedName = ctx_.makeQualifiedName(interfaceDef.getName());
            ctx_.registerGenericInterface(interfaceDef.getName(), info);
          }
        } else {
          // Precompiled stubs carry their qualified name (content-hash scoped)
          sun::QualifiedName qualifiedInterface =
              interfaceDef.hasQualifiedName()
                  ? interfaceDef.getQualifiedName()
                  : ctx_.makeQualifiedName(interfaceDef.getName());
          std::string interfaceName = qualifiedInterface.mangled();
          auto interfaceType = ctx_.types()->getInterface(interfaceName);
          if (interfaceName != interfaceDef.getName()) {
            interfaceType->setBaseName(interfaceDef.getName());
          }
          interfaceType->visibility = interfaceDef.getVisibility();
          interfaceType->setQualifiedName(qualifiedInterface);
          ctx_.registerInterface(interfaceDef.getName(), interfaceType);
        }
        break;
      }
      case ASTNodeType::CLASS_DEFINITION: {
        auto& classDef = static_cast<ClassDefinitionAST&>(*expr);
        if (classDef.isPartial()) break;
        // Skip if already registered
        if (ctx_.lookupClass(classDef.getName())) break;
        // Precompiled stubs carry their qualified name (content-hash scoped)
        sun::QualifiedName qualifiedClass =
            classDef.hasQualifiedName()
                ? classDef.getQualifiedName()
                : ctx_.makeQualifiedName(classDef.getName());
        if (classDef.isGeneric() || classDef.hasGenericMethods()) {
          GenericClassInfo genericInfo;
          genericInfo.AST = &classDef;
          genericInfo.typeParameters = classDef.getTypeParameters();
          genericInfo.definitionScope = ctx_.scope()->shared_from_this();
          genericInfo.qualifiedName = qualifiedClass;
          ctx_.registerGenericClass(classDef.getName(), genericInfo);
        }
        if (!classDef.isGeneric()) {
          auto classType = ctx_.types()->getClass(qualifiedClass);
          classType->setPacked(classDef.isPacked());
          classType->visibility = classDef.getVisibility();
          ctx_.registerClass(classDef.getName(), classType);
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
        ctx_.declareModule(nsDecl);
        collectDeclarations(const_cast<BlockExprAST&>(nsDecl.getBody()));
        ctx_.exitScope();
        break;
      }
      case ASTNodeType::MOON_SCOPE: {
        // Process the contained module stubs with content hash prefix
        auto& moonScope = static_cast<MoonScopeAST&>(*expr);
        const std::string& contentHash = moonScope.getContentHash();
        if (!contentHash.empty()) {
          ctx_.enterModuleScope(contentHash);
        }
        collectDeclarations(const_cast<BlockExprAST&>(moonScope.getBody()));
        if (!contentHash.empty()) {
          ctx_.exitScope();
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
    sun::QualifiedName qualifiedClass =
        classDef.hasQualifiedName()
            ? classDef.getQualifiedName()
            : ctx_.makeQualifiedName(classDef.getName());
    if (preRegisteredClassShapes_.count(qualifiedClass.mangled())) continue;
    auto classType = ctx_.lookupClass(classDef.getName());
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
        ctx_.enterModuleScope(nsDecl.getName());
        // Register the module's enums first: function signatures below may
        // use enum types (including generic enums like Option<i32>)
        collectEnumDeclarations(nsDecl.getBody());
        // Collect function declarations inside the module
        for (const auto& bodyExpr :
             const_cast<BlockExprAST&>(nsDecl.getBody()).getBody()) {
          if (bodyExpr->getType() == ASTNodeType::FUNCTION)
            collectFunctionSignature(static_cast<FunctionAST&>(*bodyExpr));
        }
        ctx_.exitScope();
        break;
      }
      case ASTNodeType::MOON_SCOPE: {
        // Process the contained module stubs with content hash prefix
        auto& moonScope = static_cast<MoonScopeAST&>(*expr);
        const std::string& contentHash = moonScope.getContentHash();
        if (!contentHash.empty()) {
          ctx_.enterModuleScope(contentHash);
        }
        for (const auto& bodyExpr :
             const_cast<BlockExprAST&>(moonScope.getBody()).getBody()) {
          if (bodyExpr->getType() == ASTNodeType::MODULE) {
            auto& nsDecl = static_cast<ModuleAST&>(*bodyExpr);
            ctx_.enterModuleScope(nsDecl.getName());
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
            ctx_.exitScope();
          }
        }
        if (!contentHash.empty()) {
          ctx_.exitScope();
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
void DeclarationCollector::collectFunctionSignature(FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Skip lambdas and anonymous functions
  if (proto.getName().empty()) return;

  // A pack makes a function a template even with no type parameters: its
  // arity comes from the call, so it is emitted once per argument tuple.
  if (proto.isTemplate()) {
    ctx_.registerGenericFunctionInCurrentScope(func);
    return;
  }

  std::vector<sun::TypePtr> paramTypes;
  for (auto& [argName, argType] : proto.getMutableArgs()) {
    paramTypes.push_back(sema_.types().typeAnnotationToType(argType));
  }
  sun::TypePtr returnType = sun::Types::Void();
  if (proto.hasReturnType()) {
    returnType = sema_.types().typeAnnotationToType(*proto.getReturnType());
  }

  // A C extern is scoped to its module like any other item; only its emitted
  // symbol is fixed by C. No overload suffix: C has no overloading.
  sun::QualifiedName qualifiedName = ctx_.makeQualifiedName(proto.getName());
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

  ctx_.registerFunctionInCurrentScope(qualifiedName.baseName, info);
}

void DeclarationCollector::registerPrecompiledModuleVariable(
    VariableCreationAST& varCreate) {
  sun::TypePtr type;
  if (varCreate.hasTypeAnnotation()) {
    type = sema_.types().typeAnnotationToType(*varCreate.getTypeAnnotation());
  } else if (varCreate.hasValue()) {
    // The declaration inferred its type, so the bundle kept the initializer
    // for its type alone. Codegen still only declares the symbol.
    sema_.analyzeExpr(const_cast<ExprAST&>(*varCreate.getValue()));
    type = varCreate.getValue()->getResolvedType();
  }
  if (!type) return;
  varCreate.setResolvedType(type);

  // Bare-name lookup goes through `variables`, which body analysis would
  // normally populate; there is no body to analyze here.
  const std::string& name = varCreate.getName();
  VariableInfo info{type, true, false, false};
  info.visibility = varCreate.getVisibility();
  info.isConst = varCreate.isConst();
  info.qualifiedName = varCreate.getQualifiedName();
  ctx_.scope()->variables[name] = info;

  // The stub's qualified name is already scoped by content hash; it must be
  // the one registered, since that is the symbol the bundle defines.
  ctx_.registerModuleVariable(name, varCreate.getQualifiedName().mangled(),
                              type, varCreate.getVisibility(),
                              varCreate.isConst());
}

void DeclarationCollector::registerUsing(UsingAST& usingDecl) {
  // "using A.B;" where A.B is a module name means "import all from A.B"
  std::string namespacePath = usingDecl.getNamespacePathString();
  std::string target = usingDecl.getTarget();

  if (!usingDecl.isModuleImport()) {
    std::string displayPath =
        namespacePath.empty() ? target : namespacePath + "." + target;
    if (auto* modScope = ctx_.lookupModuleScope(displayPath)) {
      ctx_.requireModuleAccessible(*modScope, usingDecl.getLocation());
      UsingImport import(displayPath, "*");
      ctx_.addUsingImport(import);
      ctx_.addImportBinding(ImportBinding::wildcard(modScope));
      return;
    }
  }

  // Normal case: import symbol or wildcard from namespace
  UsingImport import(namespacePath, target);
  ctx_.addUsingImport(import);
  if (auto* modScope = ctx_.lookupModuleScope(namespacePath)) {
    ctx_.requireModuleAccessible(*modScope, usingDecl.getLocation());
    if (import.isWildcard) {
      ctx_.addImportBinding(ImportBinding::wildcard(modScope));
    } else {
      ctx_.addImportBinding(ImportBinding(target, modScope, target));
    }
  }
}

void DeclarationCollector::registerClassShape(
    ClassDefinitionAST& classDef, const sun::QualifiedName& qualifiedClass,
    std::shared_ptr<sun::ClassType> classType) {
  std::string mangledClassName = qualifiedClass.mangled();
  if (!preRegisteredClassShapes_.insert(mangledClassName).second) return;

  // Fields
  for (const auto& field : classDef.getFields()) {
    if (classType->hasField(field.name)) {
      logAndThrowError("Field '" + field.name + "' already exists in class '" +
                           classDef.getName() + "'",
                       field.location);
    }
    sun::TypePtr fieldType = sema_.types().typeAnnotationToType(field.type);

    if constexpr (sun::Config::FORBID_REF_FIELDS_IN_CLASSES) {
      if (fieldType && fieldType->isReference()) {
        logAndThrowError("Field '" + field.name + "' in class '" +
                             classDef.getName() + "' has reference type '" +
                             fieldType->toDisplayString() +
                             "'. References cannot be stored in class "
                             "fields. Use a pointer type or store a copy.",
                         field.location);
      }
    }

    sema_.checkPackedFieldType(classDef, field, fieldType);
    classType->addField(field.name, fieldType).visibility = field.visibility;
  }

  // Implemented interfaces (fields inherited, implementation recorded)
  sema_.inheritInterfaceFields(classDef, classType);

  // Method signatures ('this' resolves against the class being shaped)
  auto savedClass = ctx_.getCurrentClass();
  ctx_.setCurrentClass(classType);
  for (const auto& methodDecl : classDef.getMethods()) {
    FunctionInfo methodInfo = sema_.getFunctionInfo(*methodDecl.function);
    PrototypeAST& proto =
        const_cast<PrototypeAST&>(methodDecl.function->getProto());
    sema_.applyFunctionInfoToProto(proto, methodInfo);
    auto& method =
        classType->addMethod(proto.getName(), methodInfo.returnType,
                             methodInfo.paramTypes, methodDecl.isConstructor,
                             proto.getTypeParameterNames(), proto.canThrow());
    method.visibility = methodVisibility(*methodDecl.function);
    method.isConst = methodDecl.isConst;
  }
  ctx_.setCurrentClass(savedClass);

  // The builtin IError predates all source, so it is registered with
  // message() returning static_ptr<u8> — the only string type that exists at
  // that point. The stdlib upgrades the contract: once sun.String is known,
  // IError.message() returns an owned String clone, and every implementation
  // compiled after this line must match that signature.
  if (ctx_.types() && qualifiedClass.baseName == "String" &&
      !qualifiedClass.owner().empty() &&
      qualifiedClass.owner().back() == "sun") {
    if (auto ierror = ctx_.types()->getInterface("IError")) {
      ierror->setMethodReturnType("message", classType);
    }
  }
}

void DeclarationCollector::collectEnumDeclarations(const BlockExprAST& block) {
  for (const auto& expr : block.getBody()) {
    if (expr->getType() != ASTNodeType::ENUM_DEFINITION) continue;
    auto& enumDef = static_cast<EnumDefinitionAST&>(*expr);
    if (enumDef.isGeneric()) {
      if (!ctx_.lookupGenericEnum(enumDef.getName())) {
        ctx_.registerGenericEnum(enumDef.getName(),
                                 {&enumDef, enumDef.getTypeParameters(),
                                  ctx_.makeQualifiedName(enumDef.getName())});
      }
      continue;
    }
    if (ctx_.lookupEnum(enumDef.getName())) continue;
    auto enumType = ctx_.types()->getEnum(enumDef.getName());
    for (const auto& variant : enumDef.getVariants()) {
      enumType->addVariant(variant.name, variant.value);
    }
    enumType->visibility = enumDef.getVisibility();
    enumType->setQualifiedName(ctx_.makeQualifiedName(enumDef.getName()));
    ctx_.registerEnum(enumDef.getName(), enumType);
  }
}
