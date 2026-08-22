// metadata_extractor.cpp — Extract module metadata as protobuf from source
// files

#include "metadata_extractor.h"

#include <llvm/Support/SHA256.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "ast.pb.h"
#include "ast_serializer.h"
#include "doc_comments.h"
#include "lowering_pass.h"
#include "moon.pb.h"
#include "parser.h"

namespace sun {

namespace {

using serialization::ASTSerializer;

// Check if a function/method is generic (has type parameters)
bool isGeneric(const PrototypeAST& proto) {
  return !proto.getTypeParameters().empty();
}

// Check if a class is generic
bool isGeneric(const ClassDefinitionAST& cls) {
  return !cls.getTypeParameters().empty();
}

// Check if an interface is generic
bool isGeneric(const InterfaceDefinitionAST& iface) {
  return !iface.getTypeParameters().empty();
}

// Clear the body of a FunctionDef proto (keep only signature)
void clearBody(ast::FunctionDef* func) { func->mutable_body()->clear_body(); }

// Clear bodies of non-generic methods in a ClassDef
void clearNonGenericBodies(ast::ClassDef* cls,
                           const ClassDefinitionAST& original) {
  const auto& methods = original.getMethods();
  for (int i = 0; i < cls->methods_size() && i < (int)methods.size(); ++i) {
    auto* method = cls->mutable_methods(i);
    const auto& origMethod = methods[i];
    // Keep body only if method itself is generic OR class is generic
    bool methodIsGeneric =
        !origMethod.function->getProto().getTypeParameters().empty();
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
    bool methodIsGeneric =
        !origMethod.function->getProto().getTypeParameters().empty();
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

    // Record `using` declarations (whole-module imports) so importers can
    // rebind them around the exported stubs
    if (stmt->getType() == ASTNodeType::USING) {
      const auto& u = static_cast<const UsingAST&>(*stmt);
      std::string path = u.getNamespacePathString();
      std::string target = u.getTarget();
      std::string full = path.empty() ? target
                         : (target == "*" ? path : path + "." + target);
      auto& metadata = collector.forModule(modulePath);
      bool seen = false;
      for (const auto& existing : metadata.usings()) {
        if (existing == full) seen = true;
      }
      if (!seen) metadata.add_usings(full);
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

    // Extract functions
    if (stmt->getType() == ASTNodeType::FUNCTION) {
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

  // Create serializer (don't include analysis data, do include locations)
  ASTSerializer serializer(
      {.include_analysis = false, .include_location = true});

  ModuleCollector collector;
  extractFromStatements(ast.getBody(), collector, "", serializer, moduleDir);

  std::vector<moon::ModuleMetadata> out;
  size_t idx = 0;
  for (auto& [name, md] : collector.modules) {
    // A file-level shell with nothing exported (just usings) is noise. Named
    // modules are kept even when empty: their visibility is part of the
    // bundle's interface (an outer `public module a` of `a.b`, say)
    bool empty = md.functions_size() == 0 && md.classes_size() == 0 &&
                 md.interfaces_size() == 0 && md.enums_size() == 0 &&
                 md.globals_size() == 0;
    if (empty && name.empty()) continue;
    // Bundle entries are keyed by source hash: several modules from one file
    // need distinct keys
    md.set_source_hash(idx == 0 ? sourceHash
                                : sourceHash + "-" + std::to_string(idx));
    ++idx;
    md.set_version("1.0.0");
    out.push_back(std::move(md));
  }
  if (out.empty()) {
    moon::ModuleMetadata md;
    md.set_source_hash(sourceHash);
    md.set_version("1.0.0");
    out.push_back(std::move(md));
  }
  return out;
}

}  // namespace

std::optional<std::vector<moon::ModuleMetadata>> extractAllMetadataFromFile(
    const std::string& filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return extractAllMetadataFromSource(
      buffer.str(), filename,
      std::filesystem::path(filename).parent_path().string());
}

std::optional<std::vector<moon::ModuleMetadata>>
extractAllMetadataFromSource(const std::string& source,
                             const std::string& displayName,
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
