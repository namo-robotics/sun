#include "semantic_analysis/passes/declaration_identity_pass.h"

#include "ast.h"
#include "ast/ast_children.h"

using sun::ast::ASTNodeType;
using sun::ast::ClassDefinitionAST;
using sun::ast::EnumDefinitionAST;
using sun::ast::ExprAST;
using sun::ast::forEachChild;
using sun::ast::InterfaceDefinitionAST;
using sun::ast::MatchExprAST;
using sun::ast::TryCatchExprAST;
using sun::support::logAndThrowError;

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {
/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/** Register one imported binder or allocate a new source binder. */
DeclarationId parameter(DeclarationTable& table, DeclarationKind kind,
                        const std::string& name, DeclarationId owner,
                        DeclarationId module, DeclarationId origin,
                        DeclarationId imported = {}) {
  return imported ? table.importedSyntax(imported.encoding(), kind, name)
                  : table.add(kind, name, owner, module, {}, origin);
}

/** Allocate identities for the parameters attached to one declaration. */
template <typename Declaration>
void parameters(const Declaration& node, DeclarationIdentity& identity,
                DeclarationTable& table, DeclarationId module,
                const DeclarationIdentity* origin) {
  if (identity.imported &&
      identity.typeParameters.size() != node.getTypeParameters().size())
    logAndThrowError(
        "Imported type parameter identities do not match the declaration");
  if (identity.imported || identity.typeParameters.empty())
    for (size_t i = 0; i < node.getTypeParameters().size(); ++i) {
      auto id = parameter(
          table, DeclarationKind::TypeParameter,
          node.getTypeParameters()[i].name, identity.id, module,
          origin ? origin->typeParameters.at(i) : DeclarationId{},
          identity.imported ? identity.typeParameters.at(i) : DeclarationId{});
      if (!identity.imported) identity.typeParameters.push_back(id);
    }
}

/** Allocate identities for lifetime parameters without resolving lifetimes. */
template <typename Declaration>
void lifetimes(const Declaration& node, DeclarationIdentity& identity,
               DeclarationTable& table, DeclarationId module,
               const DeclarationIdentity* origin) {
  if (identity.imported &&
      identity.lifetimeParameters.size() != node.getLifetimeParameters().size())
    logAndThrowError(
        "Imported lifetime identities do not match the declaration");
  if (identity.imported || identity.lifetimeParameters.empty())
    for (size_t i = 0; i < node.getLifetimeParameters().size(); ++i) {
      auto id =
          parameter(table, DeclarationKind::LifetimeParameter,
                    node.getLifetimeParameters()[i].name, identity.id, module,
                    origin ? origin->lifetimeParameters.at(i) : DeclarationId{},
                    identity.imported ? identity.lifetimeParameters.at(i)
                                      : DeclarationId{});
      if (!identity.imported) identity.lifetimeParameters.push_back(id);
    }
}

/** Register struct-backed declarations while preserving their existing IDs. */
void binding(DeclarationIdentity& identity, DeclarationKind kind,
             const std::string& name, DeclarationTable& table,
             DeclarationId owner, DeclarationId module,
             DeclarationId origin = {}) {
  if (!identity.id || (identity.imported && !identity.hasSession())) {
    identity.id = identity.imported
                      ? table.importedSyntax(identity.id.encoding(), kind, name)
                      : table.add(kind, name, owner, module, {}, origin);
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
        if (resetIdentity) field.declaration.resetSession();
      break;
    case ASTNodeType::INTERFACE_DEFINITION:
      for (const auto& field :
           static_cast<const InterfaceDefinitionAST&>(root).getFields())
        if (resetIdentity) field.declaration.resetSession();
      break;
    case ASTNodeType::ENUM_DEFINITION:
      for (const auto& variant :
           static_cast<const EnumDefinitionAST&>(root).getVariants())
        if (resetIdentity) variant.declaration.resetSession();
      break;
    case ASTNodeType::MATCH:
      for (auto& arm :
           const_cast<MatchExprAST&>(static_cast<const MatchExprAST&>(root))
               .getArmsMutable()) {
        arm.resolvedVariantTag = -1;
        for (auto& value : arm.bindings) {
          value.resolvedType.reset();
          if (resetIdentity) value.declaration.resetSession();
        }
      }
      break;
    case ASTNodeType::TRY_CATCH:
      for (auto& clause : const_cast<TryCatchExprAST&>(
                              static_cast<const TryCatchExprAST&>(root))
                              .getCatchClausesMutable()) {
        clause.isCatchAll = false;
        clause.resolvedType.reset();
        if (resetIdentity) clause.declaration.resetSession();
      }
      break;
    default:
      break;
  }
}

}  // namespace

void DeclarationIdentityPass::run(const ExprAST& root, DeclarationId owner,
                                  DeclarationId module,
                                  const ExprAST* origin) const {
  visit(root, owner, module, origin, false);
}

void DeclarationIdentityPass::run(
    const ExprAST& root, bool skipImportedMoons,
    const std::function<void()>& declarationsReady) const {
  visit(root, {}, {}, nullptr, skipImportedMoons);
  if (declarationsReady) declarationsReady();
}

void DeclarationIdentityPass::visit(const ExprAST& root, DeclarationId owner,
                                    DeclarationId module, const ExprAST* origin,
                                    bool skipImportedMoons) const {
  if (skipImportedMoons && root.getType() == ASTNodeType::MOON_SCOPE &&
      !static_cast<const sun::ast::MoonScopeAST&>(root).isOwnBundle())
    return;
  if (origin && origin->getType() != root.getType())
    logAndThrowError("Generated declaration does not match its source syntax");
  const auto* sourceIdentity = origin && origin->getDeclarationId()
                                   ? &origin->declarationIdentity()
                                   : nullptr;
  if (sourceIdentity && sourceIdentity->id &&
      sourceIdentity->session.lock() != table_.session())
    logAndThrowError(
        "Generated declaration origin belongs to another analysis session");
  auto declare = [&](DeclarationKind kind, const std::string& name) {
    auto id = root.getDeclarationId();
    auto& identity = root.declarationIdentity();
    if (!id || (identity.imported && !identity.hasSession())) {
      id = identity.imported
               ? table_.importedSyntax(identity.id.encoding(), kind, name)
           : kind == DeclarationKind::Module
               ? table_.module(name, module)
               : table_.add(
                     kind, name, owner, module, {},
                     origin ? origin->getDeclarationId() : DeclarationId{});
      root.setDeclarationId(id);
      root.declarationIdentity().session = table_.session();
      // A module may be opened in several places, so no one node declares it.
      if (kind != DeclarationKind::Module) table_.bindAstNode(id, &root);
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
      const auto& moon = static_cast<const sun::ast::MoonScopeAST&>(root);
      if (!moon.getContentHash().empty()) {
        owner = table_.module(moon.getContentHash(), module);
        root.setDeclarationId(owner);
        root.declarationIdentity().session = table_.session();
        auto hash = moon.getContentHash();
        if (hash.starts_with("$") && hash.ends_with("$"))
          hash = hash.substr(1, hash.size() - 2);
        if (hash.size() == 64 && !table_.exportedId(owner))
          table_.bindExportId(owner, DeclarationId::original(hash, 1), hash);
        module = owner;
      }
      break;
    }
    case ASTNodeType::MODULE:
      owner = declare(DeclarationKind::Module,
                      static_cast<const sun::ast::ModuleAST&>(root).getName());
      module = owner;
      break;
    case ASTNodeType::FUNCTION:
    case ASTNodeType::LAMBDA: {
      const auto& proto =
          root.getType() == ASTNodeType::FUNCTION
              ? static_cast<const sun::ast::FunctionAST&>(root).getProto()
              : static_cast<const sun::ast::LambdaAST&>(root).getProto();
      owner = declare(root.getType() == ASTNodeType::FUNCTION
                          ? DeclarationKind::Function
                          : DeclarationKind::Lambda,
                      proto.getName());
      auto& identity = proto.declarationIdentity();
      parameters(proto, identity, table_, module, sourceIdentity);
      if (proto.hasVariadicParam() && identity.variadicParameters.empty()) {
        for (size_t i = 0; i < proto.getResolvedVariadicTypes().size(); ++i)
          identity.variadicParameters.push_back(table_.add(
              DeclarationKind::Parameter,
              proto.getVariadicParam().elementName(i), owner, module, {},
              sourceIdentity
                  ? sourceIdentity->parameters.at(proto.getArgs().size())
                  : DeclarationId{},
              "variadic-element", i));
      }
      lifetimes(proto, identity, table_, module, sourceIdentity);
      const size_t parameterCount =
          proto.getArgs().size() + proto.hasVariadicParam();
      if (identity.imported && identity.parameters.size() != parameterCount)
        logAndThrowError(
            "Imported parameter identities do not match the signature");
      if (identity.imported || identity.parameters.empty()) {
        for (size_t i = 0; i < parameterCount; ++i) {
          const auto& name = i < proto.getArgs().size()
                                 ? proto.getArgs()[i].first
                                 : proto.getVariadicParamName();
          auto id = parameter(
              table_, DeclarationKind::Parameter, name, owner, module,
              sourceIdentity ? sourceIdentity->parameters.at(i)
                             : DeclarationId{},
              identity.imported ? identity.parameters.at(i) : DeclarationId{});
          if (!identity.imported) identity.parameters.push_back(id);
        }
      }
      break;
    }
    case ASTNodeType::CLASS_DEFINITION: {
      const auto& node = static_cast<const ClassDefinitionAST&>(root);
      owner = declare(DeclarationKind::Class, node.getName());
      parameters(node, node.declarationIdentity(), table_, module,
                 sourceIdentity);
      lifetimes(node, node.declarationIdentity(), table_, module,
                sourceIdentity);
      for (size_t i = 0; i < node.getFields().size(); ++i) {
        const auto& field = node.getFields()[i];
        auto source = origin ? static_cast<const ClassDefinitionAST&>(*origin)
                                   .getFields()[i]
                                   .declaration.id
                             : DeclarationId{};
        binding(field.declaration, DeclarationKind::Field, field.name, table_,
                owner, module, source);
      }
      break;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      const auto& node = static_cast<const InterfaceDefinitionAST&>(root);
      owner = declare(DeclarationKind::Interface, node.getName());
      parameters(node, node.declarationIdentity(), table_, module,
                 sourceIdentity);
      lifetimes(node, node.declarationIdentity(), table_, module,
                sourceIdentity);
      for (size_t i = 0; i < node.getFields().size(); ++i) {
        const auto& field = node.getFields()[i];
        auto source = origin
                          ? static_cast<const InterfaceDefinitionAST&>(*origin)
                                .getFields()[i]
                                .declaration.id
                          : DeclarationId{};
        binding(field.declaration, DeclarationKind::Field, field.name, table_,
                owner, module, source);
      }
      break;
    }
    case ASTNodeType::ENUM_DEFINITION: {
      const auto& node = static_cast<const EnumDefinitionAST&>(root);
      owner = declare(DeclarationKind::Enum, node.getName());
      parameters(node, node.declarationIdentity(), table_, module,
                 sourceIdentity);
      for (size_t i = 0; i < node.getVariants().size(); ++i) {
        const auto& variant = node.getVariants()[i];
        auto source = origin ? static_cast<const EnumDefinitionAST&>(*origin)
                                   .getVariants()[i]
                                   .declaration.id
                             : DeclarationId{};
        binding(variant.declaration, DeclarationKind::Variant, variant.name,
                table_, owner, module, source);
      }
      break;
    }
    case ASTNodeType::VARIABLE_CREATION:
      declare(
          DeclarationKind::Variable,
          static_cast<const sun::ast::VariableCreationAST&>(root).getName());
      break;
    case ASTNodeType::REFERENCE_CREATION:
      declare(
          DeclarationKind::Reference,
          static_cast<const sun::ast::ReferenceCreationAST&>(root).getName());
      break;
    case ASTNodeType::FOR_IN_LOOP:
      declare(DeclarationKind::Binding,
              static_cast<const sun::ast::ForInExprAST&>(root).getLoopVar());
      break;
    case ASTNodeType::DECLARE_TYPE: {
      const auto& node = static_cast<const sun::ast::DeclareTypeAST&>(root);
      if (node.hasAlias()) declare(DeclarationKind::Alias, node.getAliasName());
      break;
    }
    case ASTNodeType::MATCH: {
      const auto& arms = static_cast<const MatchExprAST&>(root).getArms();
      for (size_t i = 0; i < arms.size(); ++i)
        for (size_t j = 0; j < arms[i].bindings.size(); ++j) {
          const auto& value = arms[i].bindings[j];
          auto source = origin ? static_cast<const MatchExprAST&>(*origin)
                                     .getArms()[i]
                                     .bindings[j]
                                     .declaration.id
                               : DeclarationId{};
          if (!value.isWildcard)
            binding(value.declaration, DeclarationKind::Binding, value.name,
                    table_, owner, module, source);
        }
      break;
    }
    case ASTNodeType::TRY_CATCH: {
      const auto& clauses =
          static_cast<const TryCatchExprAST&>(root).getCatchClauses();
      for (size_t i = 0; i < clauses.size(); ++i) {
        auto source = origin ? static_cast<const TryCatchExprAST&>(*origin)
                                   .getCatchClauses()[i]
                                   .declaration.id
                             : DeclarationId{};
        binding(clauses[i].declaration, DeclarationKind::Binding,
                clauses[i].bindingName, table_, owner, module, source);
      }
      break;
    }
    default:
      break;
  }
  std::vector<const ExprAST*> sourceChildren;
  if (origin)
    forEachChild(*origin, [&](const ExprAST& child) {
      sourceChildren.push_back(&child);
    });
  size_t index = 0;
  forEachChild(root, [&](const ExprAST& child) {
    visit(child, owner, module, origin ? sourceChildren.at(index++) : nullptr,
          skipImportedMoons);
  });
}

/** Clears computed annotations throughout the tree while retaining declaration
 * identities. */
void clearComputedAnalysis(const ExprAST& root) {
  forEachChild(root,
               [](const ExprAST& child) { clearComputedAnalysis(child); });
  resetBindings(root, false);
  root.clearComputedAnalysis();
}

/** Discards analysis-session state throughout the syntax tree. */
void resetAnalysisSession(const ExprAST& root) {
  forEachChild(root, [](const ExprAST& child) { resetAnalysisSession(child); });
  resetBindings(root, true);
  root.resetAnalysisSession();
}

void DeclarationIdentityPass::run(
    const std::vector<sun::ast::MoonScopeAST*>& imports) const {
  for (auto* moon : imports) run(*moon);
}

}  // namespace sun::semantic_analysis::passes
