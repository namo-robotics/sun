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

  // Sub-pass A: Register types (enums, interfaces, classes) so that
  // function signatures can reference forward-declared types.
  for (const auto& expr : block.getBody()) {
    // Skip precompiled nodes (from .moon libs) — they are already fully
    // processed during import and must not be partially re-registered.
    if (expr->isPrecompiled()) continue;

    switch (expr->getType()) {
      case ASTNodeType::ENUM_DEFINITION: {
        auto& enumDef = static_cast<EnumDefinitionAST&>(*expr);
        // Skip if already registered (e.g. from import)
        if (lookupEnum(enumDef.getName())) break;
        // Create and register a minimal enum type
        auto enumType = typeRegistry->getEnum(enumDef.getName());
        for (const auto& variant : enumDef.getVariants()) {
          enumType->addVariant(variant.name, variant.value);
        }
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
            registerGenericInterface(interfaceDef.getName(), info);
          }
        } else {
          sun::QualifiedName qualifiedInterface =
              makeQualifiedName(interfaceDef.getName());
          std::string interfaceName = qualifiedInterface.mangled();
          auto interfaceType = typeRegistry->getInterface(interfaceName);
          if (interfaceName != interfaceDef.getName()) {
            interfaceType->setBaseName(interfaceDef.getName());
          }
          registerInterface(interfaceDef.getName(), interfaceType);
        }
        break;
      }
      case ASTNodeType::CLASS_DEFINITION: {
        auto& classDef = static_cast<ClassDefinitionAST&>(*expr);
        if (classDef.isPartial()) break;
        // Skip if already registered
        if (lookupClass(classDef.getName())) break;
        sun::QualifiedName qualifiedClass =
            makeQualifiedName(classDef.getName());
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
          registerClass(classDef.getName(), classType);
        }
        break;
      }
      case ASTNodeType::MODULE: {
        auto& nsDecl = static_cast<ModuleAST&>(*expr);
        enterModuleScope(nsDecl.getName());
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

  // Sub-pass B: Register functions (signatures only, no body analysis).
  // Types are now available for parameter/return type resolution.
  for (const auto& expr : block.getBody()) {
    // Skip precompiled nodes (from .moon libs)
    if (expr->isPrecompiled()) continue;

    switch (expr->getType()) {
      case ASTNodeType::FUNCTION: {
        auto& func = static_cast<FunctionAST&>(*expr);
        PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

        // Skip lambdas and anonymous functions
        if (proto.getName().empty()) break;

        // Register generic functions
        if (proto.isGeneric()) {
          registerGenericFunctionInCurrentScope(func);
          break;
        }

        // Resolve parameter types
        std::vector<sun::TypePtr> paramTypes;
        for (auto& [argName, argType] : proto.getMutableArgs()) {
          sun::TypePtr paramType = typeAnnotationToType(argType);
          paramTypes.push_back(paramType);
        }

        // Resolve return type
        sun::TypePtr returnType = sun::Types::Void();
        if (proto.hasReturnType()) {
          returnType = typeAnnotationToType(*proto.getReturnType());
        }

        // Compute qualified name
        sun::QualifiedName qualifiedName = makeQualifiedName(proto.getName());

        // Build minimal FunctionInfo (no captures — those require body
        // analysis)
        FunctionInfo info;
        info.returnType = returnType;
        info.paramTypes = std::move(paramTypes);
        info.qualifiedName = qualifiedName;
        info.canThrow = proto.canThrow();

        registernFunctionInCurrentScope(qualifiedName.baseName, info);
        break;
      }
      case ASTNodeType::MODULE: {
        auto& nsDecl = static_cast<ModuleAST&>(*expr);
        enterModuleScope(nsDecl.getName());
        // Collect function declarations inside the module
        for (const auto& bodyExpr :
             const_cast<BlockExprAST&>(nsDecl.getBody()).getBody()) {
          if (bodyExpr->getType() == ASTNodeType::FUNCTION) {
            auto& func = static_cast<FunctionAST&>(*bodyExpr);
            PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());
            if (proto.getName().empty()) continue;
            if (proto.isGeneric()) {
              registerGenericFunctionInCurrentScope(func);
              continue;
            }
            std::vector<sun::TypePtr> paramTypes;
            for (auto& [argName, argType] : proto.getMutableArgs()) {
              sun::TypePtr paramType = typeAnnotationToType(argType);
              paramTypes.push_back(paramType);
            }
            sun::TypePtr returnType = sun::Types::Void();
            if (proto.hasReturnType()) {
              returnType = typeAnnotationToType(*proto.getReturnType());
            }
            sun::QualifiedName qualifiedName =
                makeQualifiedName(proto.getName());
            FunctionInfo info;
            info.returnType = returnType;
            info.paramTypes = std::move(paramTypes);
            info.qualifiedName = qualifiedName;
            info.canThrow = proto.canThrow();
            registernFunctionInCurrentScope(qualifiedName.baseName, info);
          }
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
            // Collect function declarations inside the module
            for (const auto& moduleExpr :
                 const_cast<BlockExprAST&>(nsDecl.getBody()).getBody()) {
              if (moduleExpr->getType() == ASTNodeType::FUNCTION) {
                auto& func = static_cast<FunctionAST&>(*moduleExpr);
                PrototypeAST& proto =
                    const_cast<PrototypeAST&>(func.getProto());
                if (proto.getName().empty()) continue;
                if (proto.isGeneric()) {
                  registerGenericFunctionInCurrentScope(func);
                  continue;
                }
                std::vector<sun::TypePtr> paramTypes;
                for (auto& [argName, argType] : proto.getMutableArgs()) {
                  sun::TypePtr paramType = typeAnnotationToType(argType);
                  paramTypes.push_back(paramType);
                }
                sun::TypePtr returnType = sun::Types::Void();
                if (proto.hasReturnType()) {
                  returnType = typeAnnotationToType(*proto.getReturnType());
                }
                sun::QualifiedName qualifiedName =
                    makeQualifiedName(proto.getName());
                FunctionInfo info;
                info.returnType = returnType;
                info.paramTypes = std::move(paramTypes);
                info.qualifiedName = qualifiedName;
                info.canThrow = proto.canThrow();
                registernFunctionInCurrentScope(qualifiedName.baseName, info);
              }
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
