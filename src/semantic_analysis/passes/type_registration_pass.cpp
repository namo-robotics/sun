/** Registers module scopes, type names, and generic templates before
 * resolution. */
#include "semantic_analysis/passes/type_registration_pass.h"

#include "ast.h"

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ClassDefinitionAST;
using sun::ast::EnumDefinitionAST;
using sun::ast::FunctionAST;
using sun::ast::ModuleAST;
using sun::ast::MoonScopeAST;
using sun::types::Types;

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

void TypeRegistrationPass::run(BlockExprAST& block) {
  if (!ctx_.isAtModuleLevel()) return;

  for (const auto& expr : block.getBody()) {
    SemanticContext::SourceFileGuard sourceFile(ctx_, expr->getSourceFileId());
    switch (expr->getType()) {
      case ASTNodeType::ENUM_DEFINITION: {
        auto& enumDef = static_cast<EnumDefinitionAST&>(*expr);
        if (enumDef.isGeneric()) {
          if (!ctx_.scope()->findGenericEnum(enumDef.getName())) {
            ctx_.currentScope().declareGenericEnum(
                enumDef.getName(), {&enumDef, enumDef.getTypeParameters(),
                                    enumDef.getQualifiedName()});
          }
          break;
        }
        if (ctx_.lookupEnum(enumDef.getName())) break;
        auto enumType = ctx_.types()->getEnum(enumDef.getDeclarationId(),
                                              enumDef.getQualifiedName());
        for (const auto& variant : enumDef.getVariants()) {
          enumType->addVariant(variant.name, variant.value,
                               variant.declaration.id);
        }
        enumType->setBaseName(enumDef.getName());
        enumType->setUnderlyingType(
            Types::fromString(enumDef.getUnderlyingTypeName()));
        enumType->visibility = enumDef.getVisibility();
        enumType->setQualifiedName(enumDef.getQualifiedName());
        ctx_.currentScope().declareEnum(enumDef.getName(), enumType);
        break;
      }
      case ASTNodeType::INTERFACE_DEFINITION: {
        auto& interfaceDef =
            static_cast<sun::ast::InterfaceDefinitionAST&>(*expr);
        if (ctx_.lookupInterface(interfaceDef.getName())) break;
        if (interfaceDef.isGeneric()) {
          if (!ctx_.lookupGenericInterface(interfaceDef.getName())) {
            GenericInterfaceInfo info;
            info.AST = &interfaceDef;
            info.typeParameters = interfaceDef.getTypeParameters();
            info.qualifiedName = interfaceDef.getQualifiedName();
            ctx_.currentScope().declareGenericInterface(interfaceDef.getName(),
                                                        info);
          }
        } else {
          QualifiedName qualifiedInterface = interfaceDef.getQualifiedName();
          std::string interfaceName = qualifiedInterface.lookupName();
          auto interfaceType = ctx_.types()->getInterface(
              interfaceDef.getDeclarationId(), qualifiedInterface);
          if (interfaceName != interfaceDef.getName()) {
            interfaceType->setBaseName(interfaceDef.getName());
          }
          interfaceType->visibility = interfaceDef.getVisibility();
          interfaceType->setQualifiedName(qualifiedInterface);
          ctx_.currentScope().declareInterface(interfaceDef.getName(),
                                               interfaceType);
        }
        break;
      }
      case ASTNodeType::CLASS_DEFINITION: {
        auto& classDef = static_cast<ClassDefinitionAST&>(*expr);
        if (classDef.isPartial() || ctx_.lookupClass(classDef.getName())) break;
        QualifiedName qualifiedClass = classDef.getQualifiedName();
        if (classDef.isGeneric() || classDef.hasGenericMethods()) {
          GenericClassInfo genericInfo;
          genericInfo.AST = &classDef;
          genericInfo.typeParameters = classDef.getTypeParameters();
          genericInfo.definitionScope = ctx_.scope()->shared_from_this();
          genericInfo.qualifiedName = qualifiedClass;
          ctx_.currentScope().declareGenericClass(classDef.getName(),
                                                  genericInfo);
        }
        if (!classDef.isGeneric()) {
          auto classType = ctx_.types()->getClass(classDef.getDeclarationId(),
                                                  qualifiedClass);
          classType->setPacked(classDef.isPacked());
          classType->visibility = classDef.getVisibility();
          ctx_.currentScope().declareClass(classDef.getName(), classType);
        }
        break;
      }
      case ASTNodeType::FUNCTION: {
        auto& function = static_cast<FunctionAST&>(*expr);
        const auto& prototype = function.getProto();
        if (!prototype.getName().empty() && prototype.isTemplate())
          ctx_.currentScope().declareGenericFunction(function);
        break;
      }
      case ASTNodeType::MODULE: {
        auto& module = static_cast<ModuleAST&>(*expr);
        ctx_.enterScope(ctx_.currentScope().declareModule(module));
        run(const_cast<BlockExprAST&>(module.getBody()));
        ctx_.exitScope();
        break;
      }
      case ASTNodeType::MOON_SCOPE: {
        auto& moonScope = static_cast<MoonScopeAST&>(*expr);
        const std::string& contentHash = moonScope.getContentHash();
        if (!contentHash.empty()) ctx_.enterModuleScope(contentHash);
        run(const_cast<BlockExprAST&>(moonScope.getBody()));
        if (!contentHash.empty()) ctx_.exitScope();
        break;
      }
      default:
        break;
    }
  }
}

}  // namespace sun::semantic_analysis::passes
