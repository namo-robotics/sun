#include "moon_bundling/metadata_types.h"
#include "semantic_analysis/semantic_analyzer.h"
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

namespace sun {

namespace {

using serialization::ASTSerializer;

// Check if a function/method is generic (has type parameters)
bool isGeneric(const PrototypeAST& proto) { return proto.isTemplate(); }

// Check if a class is generic
bool isGeneric(const ClassDefinitionAST& cls) {
  return !cls.getTypeParameters().empty();
}

// Check if an interface is generic
bool isGeneric(const InterfaceDefinitionAST& iface) {
  return !iface.getTypeParameters().empty();
}

// Clear the body of a FunctionDef proto (keep only signature)
void clearBody(ast::FunctionDef* func) {
  func->mutable_body()->clear_body();
  func->set_field_initializer_count(0);
}

// Clear bodies of non-generic methods in a ClassDef
void clearNonGenericBodies(ast::ClassDef* cls,
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

// Clear bodies of non-generic methods in an InterfaceDef
void clearNonGenericBodies(ast::InterfaceDef* iface,
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

// Extract a function and add to metadata
void extractFunction(const FunctionAST& func, moon::ModuleMetadata& metadata,
                     const ASTSerializer& serializer) {
  // Serialize the function AST to proto
  ast::ASTNode node = serializer.serialize(func);

  // Add to metadata
  ast::FunctionDef* funcDef = metadata.add_functions();
  *funcDef = node.function_def();
  if (node.has_location()) *funcDef->mutable_location() = node.location();

  // Clear body if not generic
  if (!isGeneric(func.getProto())) {
    clearBody(funcDef);
  }
}

// Extract a class and add to metadata
void extractClass(const ClassDefinitionAST& cls, moon::ModuleMetadata& metadata,
                  const ASTSerializer& serializer) {
  // Serialize the class AST to proto
  ast::ASTNode node = serializer.serialize(cls);

  // Add to metadata
  ast::ClassDef* classDef = metadata.add_classes();
  *classDef = node.class_def();
  if (node.has_location()) *classDef->mutable_location() = node.location();

  // Clear bodies of non-generic methods
  clearNonGenericBodies(classDef, cls);
}

// Extract an interface and add to metadata
void extractInterface(const InterfaceDefinitionAST& iface,
                      moon::ModuleMetadata& metadata,
                      const ASTSerializer& serializer) {
  // Serialize the interface AST to proto
  ast::ASTNode node = serializer.serialize(iface);

  // Add to metadata
  ast::InterfaceDef* ifaceDef = metadata.add_interfaces();
  *ifaceDef = node.interface_def();
  if (node.has_location()) *ifaceDef->mutable_location() = node.location();

  // Clear bodies of non-generic methods
  clearNonGenericBodies(ifaceDef, iface);
}

// Extract a module-level variable and add to metadata.
// The initializer is dropped where the declaration states a type: this
// bundle's bitcode already holds the initialized storage, and importers
// reference that symbol rather than defining their own copy. Where the type
// was inferred the initializer is kept, since extraction runs on the parse
// tree and there is nothing else to read the type from.
void extractGlobal(const VariableCreationAST& var,
                   moon::ModuleMetadata& metadata,
                   const ASTSerializer& serializer) {
  ast::ASTNode node = serializer.serialize(var);
  ast::VariableCreation* global = metadata.add_globals();
  *global = node.variable_creation();
  if (global->has_type_annotation()) global->clear_value();
}

// Extract an enum and add to metadata
void extractEnum(const EnumDefinitionAST& enumDef,
                 moon::ModuleMetadata& metadata,
                 const ASTSerializer& serializer) {
  // Serialize the enum AST to proto
  ast::ASTNode node = serializer.serialize(enumDef);

  // Add to metadata
  ast::EnumDef* enumProto = metadata.add_enums();
  *enumProto = node.enum_def();
  if (node.has_location()) *enumProto->mutable_location() = node.location();
}

// Recursively extract from statements
// Collects definitions into one ModuleMetadata per dotted module path
// ("a.b" for `module a { module b { ... } }`); file-level definitions go
// under the empty name. Vector order = first-seen order.
struct ModuleCollector {
  std::vector<std::pair<std::string, moon::ModuleMetadata>> modules;

  moon::ModuleMetadata& forModule(const std::string& dotted) {
    for (auto& [name, md] : modules) {
      if (name == dotted) return md;
    }
    modules.emplace_back(dotted, moon::ModuleMetadata{});
    modules.back().second.set_module_name(dotted);
    return modules.back().second;
  }
};

void extractFromStatements(const std::vector<std::unique_ptr<ExprAST>>& stmts,
                           ModuleCollector& collector,
                           const std::string& modulePath,
                           const ASTSerializer& serializer,
                           const std::filesystem::path& moduleDir) {
  for (const auto& stmt : stmts) {
    if (!stmt) continue;

    // Keep source imports in their original module and file context.
    if (stmt->getType() == ASTNodeType::USING) {
      *collector.forModule(modulePath).add_using_declarations() =
          serializer.serialize(*stmt);
    }

    // Handle module/namespace blocks (nested modules become dotted paths)
    if (stmt->getType() == ASTNodeType::MODULE) {
      const auto& nsDecl = static_cast<const ModuleAST&>(*stmt);
      std::string nested = modulePath.empty()
                               ? nsDecl.getName()
                               : modulePath + "." + nsDecl.getName();
      auto& md = collector.forModule(nested);
      // Visibility is per module, agreed across all of its openings
      if (nsDecl.isPublic()) md.set_visibility(ast::PUBLIC);
      extractFromStatements(nsDecl.getBody().getBody(), collector, nested,
                            serializer, moduleDir);
    }

    // Extract functions. Tests never ship in a bundle: they are unreachable
    // from importers and would only bloat it.
    if (stmt->getType() == ASTNodeType::FUNCTION &&
        !static_cast<const FunctionAST&>(*stmt).isTest()) {
      extractFunction(static_cast<const FunctionAST&>(*stmt),
                      collector.forModule(modulePath), serializer);
    }

    // Extract classes
    if (stmt->getType() == ASTNodeType::CLASS_DEFINITION) {
      extractClass(static_cast<const ClassDefinitionAST&>(*stmt),
                   collector.forModule(modulePath), serializer);
    }

    // Extract interfaces
    if (stmt->getType() == ASTNodeType::INTERFACE_DEFINITION) {
      extractInterface(static_cast<const InterfaceDefinitionAST&>(*stmt),
                       collector.forModule(modulePath), serializer);
    }

    // Extract enums
    if (stmt->getType() == ASTNodeType::ENUM_DEFINITION) {
      extractEnum(static_cast<const EnumDefinitionAST&>(*stmt),
                  collector.forModule(modulePath), serializer);
    }

    // Extract module-level variables
    if (stmt->getType() == ASTNodeType::VARIABLE_CREATION) {
      extractGlobal(static_cast<const VariableCreationAST&>(*stmt),
                    collector.forModule(modulePath), serializer);
    }
  }
}

std::vector<moon::ModuleMetadata> extractAllMetadata(
    const std::string& filePath, const BlockExprAST& ast,
    const std::string& sourceHash) {
  std::filesystem::path moduleDir =
      std::filesystem::path(filePath).parent_path();

  ASTSerializer serializer({.include_location = true});

  ModuleCollector collector;
  extractFromStatements(ast.getBody(), collector, "", serializer, moduleDir);

  std::vector<moon::ModuleMetadata> out;
  size_t idx = 0;
  for (auto& [name, md] : collector.modules) {
    // Keep file-level imports even when all declarations are inside modules.
    // Named modules also carry visibility when otherwise empty.
    bool empty = md.functions_size() == 0 && md.classes_size() == 0 &&
                 md.interfaces_size() == 0 && md.enums_size() == 0 &&
                 md.globals_size() == 0;
    if (empty && name.empty() && md.using_declarations_size() == 0) continue;
    // Bundle entries are keyed by source hash: several modules from one file
    // need distinct keys
    md.set_source_hash(idx == 0 ? sourceHash
                                : sourceHash + "-" + std::to_string(idx));
    ++idx;
    md.set_version("1.0.0");
    md.set_source_path(filePath);
    out.push_back(std::move(md));
  }
  if (out.empty()) {
    moon::ModuleMetadata md;
    md.set_source_hash(sourceHash);
    md.set_version("1.0.0");
    md.set_source_path(filePath);
    out.push_back(std::move(md));
  }
  return out;
}

}  // namespace

std::vector<moon::ModuleMetadata> extractAnalyzedMetadata(
    const BlockExprAST& program, SemanticAnalyzer& analyzer,
    const std::string& bundleHash) {
  auto& ctx = analyzer.context();
  ASTSerializer serializer({.include_location = true});
  std::vector<moon::ModuleMetadata> result;
  std::map<std::pair<std::string, SourceFileId>, size_t> entries;
  auto entry = [&](const std::string& path,
                   const ExprAST& stmt) -> moon::ModuleMetadata& {
    auto [it, added] =
        entries.try_emplace({path, stmt.getSourceFileId()}, result.size());
    if (added) {
      auto& md = result.emplace_back();
      md.set_module_name(path);
      md.set_content_hash(bundleHash);
      md.set_source_hash(bundleHash + "-" + std::to_string(it->second));
      md.set_source_path(stmt.getLocation().filePath.value_or(""));
      md.set_version("1.0.0");
    }
    return result[it->second];
  };
  std::function<void(const BlockExprAST&, std::string, Visibility)> walk =
      [&](const BlockExprAST& block, std::string path, Visibility visibility) {
        for (const auto& stmt : block.getBody()) {
          SemanticContext::SourceFileGuard file(ctx, stmt->getSourceFileId());
          SemanticContext::LocationGuard location(ctx, stmt->getLocation());
          if (auto* moon = dynamic_cast<const MoonScopeAST*>(stmt.get())) {
            if (!moon->isOwnBundle()) continue;
            SemanticContext::ScopeSwitchGuard scope(
                ctx, ctx.lookupModuleScope(moon->getContentHash()));
            walk(moon->getBody(), "", Visibility::Private);
            continue;
          }
          if (auto* module = dynamic_cast<const ModuleAST*>(stmt.get())) {
            auto nested = path.empty() ? module->getName()
                                       : path + "." + module->getName();
            entry(nested, *module)
                .set_visibility(module->isPublic() ? ast::PUBLIC
                                                   : ast::PRIVATE);
            SemanticContext::ScopeSwitchGuard scope(
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
            extractClass(*cls, temporary, serializer);
          } else if (auto* iface = dynamic_cast<const InterfaceDefinitionAST*>(
                         stmt.get())) {
            extractInterface(*iface, temporary, serializer);
          } else if (auto* enumeration =
                         dynamic_cast<const EnumDefinitionAST*>(stmt.get())) {
            extractEnum(*enumeration, temporary, serializer);
          } else if (auto* variable =
                         dynamic_cast<const VariableCreationAST*>(stmt.get())) {
            extractGlobal(*variable, temporary, serializer);
            auto* global = temporary.mutable_globals(0);
            if (!global->has_type_annotation())
              *global->mutable_type_annotation() =
                  exportType(variable->getResolvedType());
            global->clear_value();
          } else if (stmt->getType() == ASTNodeType::USING) {
            *temporary.add_using_declarations() = serializer.serialize(*stmt);
          } else
            continue;
          bindMetadataTypes(temporary, ctx);
          bindMetadataModules(temporary, ctx);
          auto& md = entry(path, *stmt);
          md.set_visibility(visibility == Visibility::Public ? ast::PUBLIC
                                                             : ast::PRIVATE);
          md.MergeFrom(temporary);
        }
      };
  walk(program, "", Visibility::Private);
  return result;
}

std::optional<std::vector<moon::ModuleMetadata>> extractAllMetadataFromFile(
    const std::string& filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  // The bundle records where each module came from, so editors can open the
  // library source behind an imported declaration
  std::string sourcePath = filename;
  try {
    sourcePath = std::filesystem::canonical(filename).string();
  } catch (const std::filesystem::filesystem_error&) {
    sourcePath = std::filesystem::absolute(filename).string();
  }
  return extractAllMetadataFromSource(
      buffer.str(), sourcePath,
      std::filesystem::path(sourcePath).parent_path().string());
}

std::optional<std::vector<moon::ModuleMetadata>> extractAllMetadataFromSource(
    const std::string& source, const std::string& displayName,
    const std::string& baseDir) {
  // Compute SHA-256 hash of source contents
  llvm::SHA256 sha;
  sha.update(llvm::StringRef(source));
  auto hashBytes = sha.final();
  std::string sourceHash;
  sourceHash.reserve(64);
  for (uint8_t b : hashBytes) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", b);
    sourceHash += hex;
  }

  std::istringstream ss(source);
  Parser parser(ss);
  parser.setBaseDir(baseDir);
  parser.getNextToken();

  auto ast = parser.parseProgram();
  if (!ast) {
    return std::nullopt;
  }

  // Lower the parse tree so extracted generic function bodies contain only
  // core AST nodes (paren/template-string nodes never reach .moon files)
  LoweringPass lowering;
  lowering.run(*ast);

  // Doc comments ride along in the bundle so editors can show them for
  // imported declarations without the library's source at hand
  attachDocComments(*ast, source);

  return extractAllMetadata(displayName, *ast, sourceHash);
}

std::optional<moon::ModuleMetadata> extractMetadataFromFile(
    const std::string& filename) {
  auto all = extractAllMetadataFromFile(filename);
  if (!all || all->empty()) return std::nullopt;
  return (*all)[0];
}

std::optional<moon::ModuleMetadata> extractMetadataFromSource(
    const std::string& source, const std::string& displayName,
    const std::string& baseDir) {
  auto all = extractAllMetadataFromSource(source, displayName, baseDir);
  if (!all || all->empty()) return std::nullopt;
  return (*all)[0];
}

}  // namespace sun
