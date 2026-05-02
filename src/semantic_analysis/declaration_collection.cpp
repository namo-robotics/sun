// semantic_analysis/declaration_collection.cpp — Pass 1: Collect declarations
//
// This pass runs before analyzeExpr to pre-register all top-level declarations
// (functions, classes, interfaces, enums) in the current scope. This enables
// forward references: a function can call another function declared later in
// the same block.

#include "error.h"
#include "semantic_analyzer.h"

void SemanticAnalyzer::collectDeclarations(ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::FUNCTION: {
      auto& func = static_cast<FunctionAST&>(expr);
      PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

      // Skip lambdas (no name to register)
      if (proto.getName().empty()) break;

      // Validate function name
      if (isReservedIdentifier(proto.getName())) break;  // Will error in Pass 2

      // Compute qualified name from current module path (includes library hash
      // for moon imports since library scope uses hash as module name)
      sun::QualifiedName qualifiedName = makeQualifiedName(proto.getName());
      proto.setQualifiedName(qualifiedName);

      // For generic functions, register the template
      if (proto.isGeneric() && !currentClass) {
        GenericFunctionInfo genInfo;
        genInfo.AST = &func;
        genInfo.typeParameters = proto.getTypeParameters();
        if (proto.hasReturnType()) {
          genInfo.returnType = *proto.getReturnType();
        }
        genInfo.params = proto.getArgs();
        sun::QualifiedName qname(getCurrentModulePath(),
                                 getCurrentFunctionContext(), proto.getName());
        if (!scopeStack.empty()) {
          scopeStack.back().genericFunctions[qname] = genInfo;
          if (scopeStack.size() > 1) {
            scopeStack.front().genericFunctions[qname] = genInfo;
          }
        }
        break;
      }

      // Only pre-register if return type is explicitly specified.
      // We resolve param types and return type here without calling
      // getFunctionInfo (which needs full scope for captures).
      // Skip if any type fails to resolve (e.g., generic types requiring
      // 'using' that hasn't been processed yet in Pass 1).
      if (proto.hasReturnType()) {
        std::vector<sun::TypePtr> paramTypes;
        bool allResolved = true;
        for (auto& [argName, argType] : proto.getMutableArgs()) {
          auto pt = typeAnnotationToType(argType);
          if (!pt) {
            allResolved = false;
            break;
          }
          paramTypes.push_back(pt);
        }
        sun::TypePtr returnType =
            allResolved ? typeAnnotationToType(*proto.getReturnType())
                        : nullptr;
        if (allResolved && returnType) {
          FunctionInfo funcInfo{returnType, paramTypes, {}};
          registerFunction(qualifiedName.mangled(), funcInfo);
        }
      }
      break;
    }

    case ASTNodeType::CLASS_DEFINITION: {
      auto& classDef = static_cast<ClassDefinitionAST&>(expr);
      std::string className = qualifyNameInCurrentModule(classDef.getName());

      // Generic classes: register template only
      if (classDef.isGeneric()) {
        GenericClassInfo genericInfo;
        genericInfo.AST = &classDef;
        genericInfo.typeParameters = classDef.getTypeParameters();
        registerGenericClass(className, genericInfo);
        break;
      }

      // Check for generic methods — register in genericClassTable too
      bool hasGenericMethods = false;
      for (const auto& methodDecl : classDef.getMethods()) {
        if (methodDecl.function->getProto().isGeneric()) {
          hasGenericMethods = true;
          break;
        }
      }
      if (hasGenericMethods) {
        GenericClassInfo genericInfo;
        genericInfo.AST = &classDef;
        genericInfo.typeParameters = {};
        registerGenericClass(className, genericInfo);
      }

      // Create and register the class type so other declarations can reference
      // it. Fields and methods will be fully populated in Pass 2.
      auto classType = typeRegistry->getClass(className);
      if (className != classDef.getName()) {
        classType->setBaseName(classDef.getName());
      }
      registerClass(className, classType);
      break;
    }

    case ASTNodeType::INTERFACE_DEFINITION: {
      auto& interfaceDef = static_cast<InterfaceDefinitionAST&>(expr);
      std::string ifaceName =
          qualifyNameInCurrentModule(interfaceDef.getName());

      if (interfaceDef.isGeneric()) {
        GenericInterfaceInfo genericInfo;
        genericInfo.AST = &interfaceDef;
        genericInfo.typeParameters = interfaceDef.getTypeParameters();
        // Generic interfaces register with unqualified name (matches Pass 2)
        registerGenericInterface(interfaceDef.getName(), genericInfo);
        auto interfaceType = typeRegistry->getGenericInterface(
            interfaceDef.getName(), interfaceDef.getTypeParameters());
        registerInterface(interfaceDef.getName(), interfaceType);
        break;
      }

      // Create via typeRegistry (idempotent) and register
      auto interfaceType = typeRegistry->getInterface(ifaceName);
      if (ifaceName != interfaceDef.getName()) {
        interfaceType->setBaseName(interfaceDef.getName());
      }
      registerInterface(ifaceName, interfaceType);
      break;
    }

    case ASTNodeType::ENUM_DEFINITION: {
      auto& enumDef = static_cast<EnumDefinitionAST&>(expr);
      std::string enumName = qualifyNameInCurrentModule(enumDef.getName());

      // Create via typeRegistry (idempotent) and register
      auto enumType = typeRegistry->getEnum(enumName);
      registerEnum(enumName, enumType);
      break;
    }

    case ASTNodeType::NAMESPACE: {
      auto& nsDecl = static_cast<NamespaceAST&>(expr);
      enterModuleScope(nsDecl.getName());

      // Register module name
      std::string modulePrefix = getCurrentModulePrefix();
      if (!modulePrefix.empty() && modulePrefix.back() == '_') {
        modulePrefix.pop_back();
      }
      registerModule(modulePrefix);

      // Recurse into namespace body
      for (const auto& bodyExpr : nsDecl.getBody().getBody()) {
        collectDeclarations(*bodyExpr);
      }

      exitScope();
      break;
    }

    case ASTNodeType::USING:
      // Using statements are NOT processed in Pass 1 — they are handled in
      // Pass 2 only. Processing them here would enable generic type resolution
      // in function signatures, which can trigger instantiateGenericClass.
      // That in turn analyzes method bodies that depend on class methods
      // populated in Pass 2, causing failures or dirty scope state on error.
      break;

    case ASTNodeType::IMPORT_SCOPE: {
      // Recurse into expanded import scopes to collect their declarations
      auto& importScope = static_cast<ImportScopeAST&>(expr);
      importScopeDepth_++;
      for (const auto& bodyExpr : importScope.getBody().getBody()) {
        collectDeclarations(const_cast<ExprAST&>(*bodyExpr));
      }
      importScopeDepth_--;
      break;
    }

    default:
      // Other node types (variables, expressions, imports, etc.)
      // don't need pre-declaration — handled in Pass 2.
      break;
  }
}
