#include "moon_bundling/metadata_types.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_registry.h"
// metadata_extractor.cpp — Extract module metadata as protobuf from source
// files

#include <llvm/Support/SHA256.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "ast.pb.h"
#include "moon.pb.h"
#include "moon_bundling/metadata_extractor.h"
#include "parsing/doc_comments.h"
#include "parsing/lowering_pass.h"
#include "parsing/parser.h"
#include "serialization/ast_serializer.h"

using sun::semantic_analysis::DeclarationId;
using sun::semantic_analysis::PortableDeclarationKey;
using sun::semantic_analysis::Visibility;

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ClassDefinitionAST;
using sun::ast::EnumDefinitionAST;
using sun::ast::FunctionAST;
using sun::ast::InterfaceDefinitionAST;
using sun::ast::VariableCreationAST;
using sun::semantic_analysis::SemanticContext;

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {
namespace pbc = sun::proto::ast;

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

using sun::serialization::ASTSerializer;

/**
 * Check if a function/method is generic (has type parameters)
 */
bool isGeneric(const sun::ast::PrototypeAST& proto) {
  return proto.isTemplate();
}

/**
 * Check if a class is generic
 */
bool isGeneric(const ClassDefinitionAST& cls) {
  return !cls.getTypeParameters().empty();
}

/**
 * Check if an interface is generic
 */
bool isGeneric(const InterfaceDefinitionAST& iface) {
  return !iface.getTypeParameters().empty();
}

/**
 * Clear the body of a FunctionDef proto (keep only signature)
 */
void clearBody(pbc::FunctionDef* func) {
  func->mutable_body()->clear_body();
  func->set_field_initializer_count(0);
}

/**
 * Clear bodies of non-generic methods in a ClassDef
 */
void clearNonGenericBodies(pbc::ClassDef* cls,
                           const ClassDefinitionAST& original) {
  const auto& methods = original.getMethods();
  for (int i = 0; i < cls->methods_size() && i < (int)methods.size(); ++i) {
    auto* method = cls->mutable_methods(i);
    const auto& origMethod = methods[i];
    // Keep body only if method itself is generic OR class is generic
    bool methodIsGeneric = origMethod.function->getProto().isTemplate();
    bool classIsGeneric = !original.getTypeParameters().empty();
    if (!methodIsGeneric && !classIsGeneric) {
      clearBody(method->mutable_function());
    }
  }
}

/**
 * Clear bodies of non-generic methods in an InterfaceDef
 */
void clearNonGenericBodies(pbc::InterfaceDef* iface,
                           const InterfaceDefinitionAST& original) {
  const auto& methods = original.getMethods();
  for (int i = 0; i < iface->methods_size() && i < (int)methods.size(); ++i) {
    auto* method = iface->mutable_methods(i);
    const auto& origMethod = methods[i];
    // Keep body only if method itself is generic OR interface is generic
    bool methodIsGeneric = origMethod.function->getProto().isTemplate();
    bool ifaceIsGeneric = !original.getTypeParameters().empty();
    if (!methodIsGeneric && !ifaceIsGeneric) {
      clearBody(method->mutable_function());
    }
  }
}

/**
 * Extract a function and add to metadata
 */
void extractFunction(const FunctionAST& func, moon::ModuleMetadata& metadata,
                     const ASTSerializer& serializer) {
  if (func.isExtern() && func.getTargetDeclarationId()) return;
  // Serialize the function AST to proto
  pbc::ASTNode node = serializer.serialize(func);

  // Add to metadata
  pbc::FunctionDef* funcDef = metadata.add_functions();
  *funcDef = node.function_def();
  if (node.has_location()) *funcDef->mutable_location() = node.location();

  // Clear body if not generic
  if (!isGeneric(func.getProto())) {
    clearBody(funcDef);
  }
}

/**
 * Extract a class and add to metadata
 */
void extractClass(
    const ClassDefinitionAST& cls, moon::ModuleMetadata& metadata,
    const ASTSerializer& serializer,
    const sun::semantic_analysis::AnalysisResults* analysis = nullptr) {
  // Serialize the class AST to proto
  pbc::ASTNode node = serializer.serialize(cls);

  // Add to metadata
  pbc::ClassDef* classDef = metadata.add_classes();
  *classDef = node.class_def();
  if (node.has_location()) *classDef->mutable_location() = node.location();

  // The writer verifies these candidates against the emitted code.
  for (const auto& [instanceId, specialization] : cls.getSpecializations()) {
    if (!specialization || !analysis) continue;
    const auto& declarations = analysis->declarations;
    auto* candidate = classDef->add_compiled_specializations();
    candidate->set_declaration_key(
        PortableDeclarationKey::fromDeclaration(instanceId, declarations)
            .encoding());
    for (const auto& method :
         analysis->types->getClass(instanceId)->getMethods()) {
      if (method.isGeneric()) continue;
      candidate->add_method_symbols(PortableDeclarationKey::fromDeclaration(
                                        method.declarationId, declarations)
                                        .symbol("function"));
    }
  }

  // Clear bodies of non-generic methods
  clearNonGenericBodies(classDef, cls);
}

/**
 * Extract an interface and add to metadata
 */
void extractInterface(const InterfaceDefinitionAST& iface,
                      moon::ModuleMetadata& metadata,
                      const ASTSerializer& serializer) {
  // Serialize the interface AST to proto
  pbc::ASTNode node = serializer.serialize(iface);

  // Add to metadata
  pbc::InterfaceDef* ifaceDef = metadata.add_interfaces();
  *ifaceDef = node.interface_def();
  if (node.has_location()) *ifaceDef->mutable_location() = node.location();

  // Clear bodies of non-generic methods
  clearNonGenericBodies(ifaceDef, iface);
}

/**
 * Extract a module-level variable and add to metadata.
 * The initializer is dropped: this bundle's bitcode already holds the
 * initialized storage, and importers reference that symbol rather than
 * defining their own copy. What an importer does get is the type, written out
 * when the source left it to inference, and the value of a `const` that was
 * computed at compile time, so the importer can compute with it too.
 */
void extractGlobal(
    const VariableCreationAST& var, moon::ModuleMetadata& metadata,
    const ASTSerializer& serializer,
    const sun::semantic_analysis::DeclarationTable& declarations) {
  pbc::ASTNode node = serializer.serialize(var);
  pbc::VariableCreation* global = metadata.add_globals();
  *global = node.variable_creation();
  if (!global->has_type_annotation())
    *global->mutable_type_annotation() =
        exportType(var.getResolvedType(), declarations);
  global->clear_value();

  using sun::semantic_analysis::constants::GlobalInitKind;
  const auto* decision = var.getGlobalInit();
  if (var.isConst() && decision && decision->kind == GlobalInitKind::Image &&
      decision->value)
    serializer.serializeConstantValue(*decision->value,
                                      global->mutable_constant_value());
}

/**
 * Extract an enum and add to metadata
 */
void extractEnum(const EnumDefinitionAST& enumDef,
                 moon::ModuleMetadata& metadata,
                 const ASTSerializer& serializer) {
  // Serialize the enum AST to proto
  pbc::ASTNode node = serializer.serialize(enumDef);

  // Add to metadata
  pbc::EnumDef* enumProto = metadata.add_enums();
  *enumProto = node.enum_def();
  if (node.has_location()) *enumProto->mutable_location() = node.location();
}

}  // namespace

/** Exports library declaration metadata from an analyzed program. */
std::vector<moon::ModuleMetadata> extractAnalyzedMetadata(
    const BlockExprAST& program,
    sun::semantic_analysis::SemanticAnalyzer& analyzer,
    const std::string& bundleHash) {
  auto& ctx = analyzer.context();
  auto& declarations = ctx.results().declarations;
  PortableDeclarationKey::assignOriginals(program, declarations, bundleHash);
  ASTSerializer serializer(
      {.declarations = &declarations, .include_location = true});
  std::vector<moon::ModuleMetadata> result;
  std::map<std::pair<std::string, sun::support::SourceFileId>, size_t> entries;
  std::map<std::string, DeclarationId> moduleIdentities;
  auto entry = [&](const std::string& path,
                   const sun::ast::ExprAST& stmt) -> moon::ModuleMetadata& {
    auto [it, added] =
        entries.try_emplace({path, stmt.getSourceFileId()}, result.size());
    if (added) {
      auto& md = result.emplace_back();
      md.set_module_name(path);
      md.set_content_hash(bundleHash);
      md.set_source_hash(bundleHash + "-" + std::to_string(it->second));
      md.set_source_path(stmt.getLocation().filePath.value_or(""));
      md.set_version("1.0.0");
      auto module = moduleIdentities[path];
      std::vector<std::string> modules;
      while (module) {
        const auto& record = declarations.get(module);
        if (record.name.starts_with("$")) break;
        modules.push_back(
            PortableDeclarationKey::fromDeclaration(module, declarations)
                .encoding());
        module = record.owner;
      }
      for (auto it = modules.rbegin(); it != modules.rend(); ++it)
        md.add_module_declarations(*it);
    }
    return result[it->second];
  };
  std::function<void(const BlockExprAST&, std::string, Visibility)> walk =
      [&](const BlockExprAST& block, std::string path, Visibility visibility) {
        for (const auto& stmt : block.getBody()) {
          sun::semantic_analysis::SemanticContext::SourceFileGuard file(
              ctx, stmt->getSourceFileId());
          sun::semantic_analysis::SemanticContext::LocationGuard location(
              ctx, stmt->getLocation());
          if (auto* moon =
                  dynamic_cast<const sun::ast::MoonScopeAST*>(stmt.get())) {
            if (!moon->isOwnBundle()) continue;
            sun::semantic_analysis::SemanticContext::ScopeSwitchGuard scope(
                ctx, ctx.lookupModuleScope(moon->getContentHash()));
            walk(moon->getBody(), "", Visibility::Private);
            continue;
          }
          if (auto* module =
                  dynamic_cast<const sun::ast::ModuleAST*>(stmt.get())) {
            auto nested = path.empty() ? module->getName()
                                       : path + "." + module->getName();
            moduleIdentities[nested] = module->getDeclarationId();
            entry(nested, *module)
                .set_visibility(module->isPublic() ? pbc::PUBLIC
                                                   : pbc::PRIVATE);
            sun::semantic_analysis::SemanticContext::ScopeSwitchGuard scope(
                ctx, ctx.scope()->childModules.at(module->getName()).get());
            walk(module->getBody(), nested, module->getVisibility());
            continue;
          }
          moon::ModuleMetadata temporary;
          if (auto* function = dynamic_cast<const FunctionAST*>(stmt.get())) {
            if (function->isTest()) continue;
            extractFunction(*function, temporary, serializer);
          } else if (auto* cls =
                         dynamic_cast<const ClassDefinitionAST*>(stmt.get())) {
            extractClass(*cls, temporary, serializer, &ctx.results());
          } else if (auto* iface = dynamic_cast<const InterfaceDefinitionAST*>(
                         stmt.get())) {
            extractInterface(*iface, temporary, serializer);
          } else if (auto* enumeration =
                         dynamic_cast<const EnumDefinitionAST*>(stmt.get())) {
            extractEnum(*enumeration, temporary, serializer);
          } else if (auto* variable =
                         dynamic_cast<const VariableCreationAST*>(stmt.get())) {
            extractGlobal(*variable, temporary, serializer, declarations);
          } else if (stmt->getType() == ASTNodeType::USING) {
            *temporary.add_using_declarations() = serializer.serialize(*stmt);
          } else
            continue;
          bindMetadataTypes(temporary, ctx);
          bindMetadataModules(temporary, ctx);
          auto& md = entry(path, *stmt);
          md.set_visibility(visibility == Visibility::Public ? pbc::PUBLIC
                                                             : pbc::PRIVATE);
          md.MergeFrom(temporary);
        }
      };
  walk(program, "", Visibility::Private);
  if (!result.empty()) {
    std::map<PortableDeclarationKey, DeclarationId> originals;
    for (size_t i = 1; i <= declarations.size(); ++i) {
      const auto id = DeclarationId(i);
      const auto& record = declarations.get(id);
      if (record.portableKey &&
          record.portableKey->encoding().substr(17, 64) == bundleHash)
        originals.emplace(*record.portableKey, id);
    }
    auto key = [&](DeclarationId id) {
      return id ? PortableDeclarationKey::fromDeclaration(id, declarations)
                      .encoding()
                : std::string{};
    };
    for (const auto& [portable, id] : originals) {
      const auto& record = declarations.get(id);
      auto* out = result.front().add_declarations();
      out->set_key(portable.encoding());
      out->set_kind(static_cast<uint32_t>(record.kind));
      out->set_name(record.name);
      out->set_owner(key(record.owner));
      out->set_module(key(record.module));
    }
  }
  return result;
}

}  // namespace sun::moon_bundling
