// metadata_extractor.cpp — Extract module metadata as protobuf from source files

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
void clearBody(ast::FunctionDef* func) {
  func->mutable_body()->clear_body();
  func->clear_source_text();
}

// Clear bodies of non-generic methods in a ClassDef
void clearNonGenericBodies(ast::ClassDef* cls,
                           const ClassDefinitionAST& original) {
  const auto& methods = original.getMethods();
  for (int i = 0; i < cls->methods_size() && i < (int)methods.size(); ++i) {
    auto* method = cls->mutable_methods(i);
    const auto& origMethod = methods[i];
    // Keep body only if method itself is generic OR class is generic
    bool methodIsGeneric = !origMethod.function->getProto().getTypeParameters().empty();
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
    bool methodIsGeneric = !origMethod.function->getProto().getTypeParameters().empty();
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

// Extract an enum and add to metadata
void extractEnum(const EnumDefinitionAST& enumDef, moon::ModuleMetadata& metadata,
                 const ASTSerializer& serializer) {
  // Serialize the enum AST to proto
  ast::ASTNode node = serializer.serialize(enumDef);
  
  // Add to metadata
  ast::EnumDef* enumProto = metadata.add_enums();
  *enumProto = node.enum_def();
}

// Recursively extract from statements
void extractFromStatements(const std::vector<std::unique_ptr<ExprAST>>& stmts,
                           moon::ModuleMetadata& metadata,
                           const ASTSerializer& serializer,
                           const std::filesystem::path& moduleDir,
                           bool isTopLevel) {
  for (const auto& stmt : stmts) {
    if (!stmt) continue;

    // Track dependencies from imports (only at top level)
    if (stmt->isImport() && isTopLevel) {
      const auto& importStmt = static_cast<const ImportAST&>(*stmt);
      std::filesystem::path depPath = importStmt.getPath();
      if (depPath.is_relative()) {
        depPath = std::filesystem::weakly_canonical(moduleDir / depPath);
      }
      metadata.add_dependencies(depPath.string());
    }

    // Handle module/namespace blocks
    if (stmt->getType() == ASTNodeType::MODULE) {
      const auto& nsDecl = static_cast<const ModuleAST&>(*stmt);
      if (isTopLevel && metadata.module_name().empty()) {
        metadata.set_module_name(nsDecl.getName());
      }
      // Recurse into module body
      extractFromStatements(nsDecl.getBody().getBody(), metadata, serializer,
                            moduleDir, false);
    }

    // Extract functions
    if (stmt->getType() == ASTNodeType::FUNCTION) {
      extractFunction(static_cast<const FunctionAST&>(*stmt), metadata,
                      serializer);
    }

    // Extract classes
    if (stmt->getType() == ASTNodeType::CLASS_DEFINITION) {
      extractClass(static_cast<const ClassDefinitionAST&>(*stmt), metadata,
                   serializer);
    }

    // Extract interfaces
    if (stmt->getType() == ASTNodeType::INTERFACE_DEFINITION) {
      extractInterface(static_cast<const InterfaceDefinitionAST&>(*stmt),
                       metadata, serializer);
    }

    // Extract enums
    if (stmt->getType() == ASTNodeType::ENUM_DEFINITION) {
      extractEnum(static_cast<const EnumDefinitionAST&>(*stmt), metadata,
                  serializer);
    }
  }
}

moon::ModuleMetadata extractMetadata(const std::string& filePath,
                                     const BlockExprAST& ast,
                                     const std::string& sourceHash) {
  moon::ModuleMetadata metadata;
  metadata.set_source_hash(sourceHash);
  metadata.set_version("1.0.0");

  std::filesystem::path moduleDir =
      std::filesystem::path(filePath).parent_path();

  // Create serializer (don't include analysis data, do include locations)
  ASTSerializer serializer({.include_analysis = false, .include_location = true});

  extractFromStatements(ast.getBody(), metadata, serializer, moduleDir, true);

  return metadata;
}

}  // namespace

std::optional<moon::ModuleMetadata> extractMetadataFromFile(
    const std::string& filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

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
  parser.setBaseDir(std::filesystem::path(filename).parent_path().string());
  parser.getNextToken();

  auto ast = parser.parseProgram();
  if (!ast) {
    return std::nullopt;
  }

  return extractMetadata(filename, *ast, sourceHash);
}

}  // namespace sun
