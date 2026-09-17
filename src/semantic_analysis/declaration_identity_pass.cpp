#include "semantic_analysis/declaration_identity_pass.h"

#include "ast.h"
#include "ast/ast_children.h"

namespace sun {
namespace {

/** Allocate identities for the parameters attached to one declaration. */
template <typename Declaration>
void parameters(const Declaration& node, DeclarationIdentity& identity,
                DeclarationTable& table, DeclarationId module) {
  if (identity.typeParameters.empty())
    for (const auto& param : node.getTypeParameters())
      identity.typeParameters.push_back(table.add(
          DeclarationKind::TypeParameter, param.name, identity.id, module));
}

/** Allocate identities for lifetime parameters without resolving lifetimes. */
template <typename Declaration>
void lifetimes(const Declaration& node, DeclarationIdentity& identity,
               DeclarationTable& table, DeclarationId module) {
  if (identity.lifetimeParameters.empty())
    for (const auto& param : node.getLifetimeParameters())
      identity.lifetimeParameters.push_back(table.add(
          DeclarationKind::LifetimeParameter, param.name, identity.id, module));
}

/** Register struct-backed declarations while preserving their existing IDs. */
void binding(DeclarationIdentity& identity, DeclarationKind kind,
             const std::string& name, DeclarationTable& table,
             DeclarationId owner, DeclarationId module) {
  if (!identity.id) {
    identity.id = table.add(kind, name, owner, module);
    identity.session = table.session();
  } else {
    if (identity.session.lock() != table.session())
      logAndThrowError(
          "Reset declaration annotations before starting another analysis "
          "session");
    table.get(identity.id);
  }
}

/** Reset annotations that are stored on declarations outside expression nodes.
 */
void resetBindings(const ExprAST& root, bool resetIdentity) {
  switch (root.getType()) {
    case ASTNodeType::CLASS_DEFINITION:
      for (const auto& field :
           static_cast<const ClassDefinitionAST&>(root).getFields())
        if (resetIdentity) field.declaration = {};
      break;
    case ASTNodeType::INTERFACE_DEFINITION:
      for (const auto& field :
           static_cast<const InterfaceDefinitionAST&>(root).getFields())
        if (resetIdentity) field.declaration = {};
      break;
    case ASTNodeType::ENUM_DEFINITION:
      for (const auto& variant :
           static_cast<const EnumDefinitionAST&>(root).getVariants())
        if (resetIdentity) variant.declaration = {};
      break;
    case ASTNodeType::MATCH:
      for (auto& arm :
           const_cast<MatchExprAST&>(static_cast<const MatchExprAST&>(root))
               .getArmsMutable()) {
        arm.resolvedVariantTag = -1;
        for (auto& value : arm.bindings) {
          value.resolvedType.reset();
          value.resolvedMangledName.clear();
          if (resetIdentity) value.declaration = {};
        }
      }
      break;
    case ASTNodeType::TRY_CATCH:
      for (auto& clause : const_cast<TryCatchExprAST&>(
                              static_cast<const TryCatchExprAST&>(root))
                              .getCatchClausesMutable()) {
        clause.isCatchAll = false;
        clause.resolvedMangledName.clear();
        if (resetIdentity) clause.declaration = {};
      }
      break;
    default:
      break;
  }
}

}  // namespace

void DeclarationIdentityPass::run(const ExprAST& root, DeclarationId owner,
                                  DeclarationId module) const {
  auto declare = [&](DeclarationKind kind, const std::string& name) {
    auto id = root.getDeclarationId();
    if (!id) {
      id = kind == DeclarationKind::Module
               ? table_.module(name, module)
               : table_.add(kind, name, owner, module);
      root.setDeclarationId(id);
      root.declarationIdentity().session = table_.session();
    } else {
      if (root.declarationIdentity().session.lock() != table_.session())
        logAndThrowError(
            "Reset declaration annotations before starting another analysis "
            "session");
      module = table_.get(id).module;
    }
    return id;
  };
  switch (root.getType()) {
    case ASTNodeType::MOON_SCOPE: {
      const auto& moon = static_cast<const MoonScopeAST&>(root);
      if (!moon.getContentHash().empty()) {
        owner = table_.module(moon.getContentHash(), module);
        module = owner;
      }
      break;
    }
    case ASTNodeType::MODULE:
      owner = declare(DeclarationKind::Module,
                      static_cast<const ModuleAST&>(root).getName());
      module = owner;
      break;
    case ASTNodeType::FUNCTION:
    case ASTNodeType::LAMBDA: {
      const auto& proto = root.getType() == ASTNodeType::FUNCTION
                              ? static_cast<const FunctionAST&>(root).getProto()
                              : static_cast<const LambdaAST&>(root).getProto();
      owner = declare(root.getType() == ASTNodeType::FUNCTION
                          ? DeclarationKind::Function
                          : DeclarationKind::Lambda,
                      proto.getName());
      auto& identity = proto.declarationIdentity();
      parameters(proto, identity, table_, module);
      lifetimes(proto, identity, table_, module);
      if (identity.parameters.empty()) {
        for (const auto& arg : proto.getArgs())
          identity.parameters.push_back(
              table_.add(DeclarationKind::Parameter, arg.first, owner, module));
        if (proto.hasVariadicParam())
          identity.parameters.push_back(table_.add(DeclarationKind::Parameter,
                                                   proto.getVariadicParamName(),
                                                   owner, module));
      }
      break;
    }
    case ASTNodeType::CLASS_DEFINITION: {
      const auto& node = static_cast<const ClassDefinitionAST&>(root);
      owner = declare(DeclarationKind::Class, node.getName());
      parameters(node, node.declarationIdentity(), table_, module);
      lifetimes(node, node.declarationIdentity(), table_, module);
      for (const auto& field : node.getFields())
        binding(field.declaration, DeclarationKind::Field, field.name, table_,
                owner, module);
      break;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      const auto& node = static_cast<const InterfaceDefinitionAST&>(root);
      owner = declare(DeclarationKind::Interface, node.getName());
      parameters(node, node.declarationIdentity(), table_, module);
      lifetimes(node, node.declarationIdentity(), table_, module);
      for (const auto& field : node.getFields())
        binding(field.declaration, DeclarationKind::Field, field.name, table_,
                owner, module);
      break;
    }
    case ASTNodeType::ENUM_DEFINITION: {
      const auto& node = static_cast<const EnumDefinitionAST&>(root);
      owner = declare(DeclarationKind::Enum, node.getName());
      parameters(node, node.declarationIdentity(), table_, module);
      for (const auto& variant : node.getVariants())
        binding(variant.declaration, DeclarationKind::Variant, variant.name,
                table_, owner, module);
      break;
    }
    case ASTNodeType::VARIABLE_CREATION:
      declare(DeclarationKind::Variable,
              static_cast<const VariableCreationAST&>(root).getName());
      break;
    case ASTNodeType::REFERENCE_CREATION:
      declare(DeclarationKind::Reference,
              static_cast<const ReferenceCreationAST&>(root).getName());
      break;
    case ASTNodeType::FOR_IN_LOOP:
      declare(DeclarationKind::Binding,
              static_cast<const ForInExprAST&>(root).getLoopVar());
      break;
    case ASTNodeType::DECLARE_TYPE: {
      const auto& node = static_cast<const DeclareTypeAST&>(root);
      if (node.hasAlias()) declare(DeclarationKind::Alias, node.getAliasName());
      break;
    }
    case ASTNodeType::MATCH:
      for (const auto& arm : static_cast<const MatchExprAST&>(root).getArms())
        for (const auto& value : arm.bindings)
          if (!value.isWildcard)
            binding(value.declaration, DeclarationKind::Binding, value.name,
                    table_, owner, module);
      break;
    case ASTNodeType::TRY_CATCH:
      for (const auto& clause :
           static_cast<const TryCatchExprAST&>(root).getCatchClauses())
        binding(clause.declaration, DeclarationKind::Binding,
                clause.bindingName, table_, owner, module);
      break;
    default:
      break;
  }
  forEachChild(root, [&](const ExprAST& child) { run(child, owner, module); });
}

void clearComputedAnalysis(const ExprAST& root) {
  forEachChild(root,
               [](const ExprAST& child) { clearComputedAnalysis(child); });
  resetBindings(root, false);
  root.clearComputedAnalysis();
}

void resetAnalysisSession(const ExprAST& root) {
  forEachChild(root, [](const ExprAST& child) { resetAnalysisSession(child); });
  resetBindings(root, true);
  root.resetAnalysisSession();
}

}  // namespace sun
