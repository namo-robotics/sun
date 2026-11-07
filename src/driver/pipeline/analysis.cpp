/** Loads imports and performs semantic analysis. */
#include <filesystem>
#include <fstream>
#include <sstream>

#include "abi/abi_compatibility_checker.h"
#include "ast/ast_children.h"
#include "driver/driver.h"
#include "moon_bundling/library_cache.h"
#include "parsing/lowering_pass.h"
#include "semantic_analysis/semantic_pipeline.h"
#include "serialization/ast_serializer.h"
#include "stdlib_support.h"
#include "support/error.h"
#include "support/stage_timer.h"

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::ModuleAST;
using sun::moon_bundling::LibraryCache;
using sun::moon_bundling::MoonImport;
using sun::parsing::Parser;
using sun::support::logAndThrowError;
using sun::support::ScopedStage;

/** Coordinates compilation, dependency loading, linking, and execution. */
namespace sun::driver {

/**
 * Check if stdlib.moon is included in moon imports
 */
bool hasStdlibImport(const std::vector<MoonImport>& moonImports) {
  for (const auto& moonImport : moonImports) {
    // Check if the path ends with stdlib.moon
    if (moonImport.path.find("stdlib.moon") != std::string::npos) {
      return true;
    }
  }
  return false;
}

/**
 * Does this block declare `class String` directly inside `module std`?
 * Interpolation desugars to `std.String` and `std.HeapAllocator`, so the
 * stdlib's own sources satisfy it without importing stdlib.moon — which
 * they cannot do, being that library.
 */
bool declaresStdlibString(const BlockExprAST& block) {
  for (const auto& stmt : block.getBody()) {
    if (!stmt || stmt->getType() != ASTNodeType::MODULE) {
      continue;
    }
    const auto& module = static_cast<const ModuleAST&>(*stmt);
    if (module.getName() != "std") {
      continue;
    }
    for (const auto& member : module.getBody().getBody()) {
      if (!member || member->getType() != ASTNodeType::CLASS_DEFINITION) {
        continue;
      }
      if (static_cast<const sun::ast::ClassDefinitionAST&>(*member).getName() ==
          "String") {
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Moon import processing
// ---------------------------------------------------------------------------

/** Validates listed bundles, collects their explicit and embedded scopes, and
 * retains their readers for linking. Embedded scopes are deduplicated and
 * hidden from source lookup; explicit scopes preserve manifest aliases.
 */
static std::vector<std::unique_ptr<sun::ast::MoonScopeAST>> loadMoonScopes(
    Parser& parser, const std::vector<MoonImport>& moonImports) {
  // Resolve the complete graph before any bundle declarations enter the AST.
  std::vector<std::unique_ptr<sun::moon_bundling::MoonReader>> readers;
  std::vector<sun::moon_bundling::MoonReader*> snapshots;
  for (const auto& moonImport : moonImports) {
    readers.push_back(parser.openMoonImport(moonImport));
    snapshots.push_back(readers.back().get());
  }
  abi::AbiCompatibilityChecker::current().validateImports(snapshots,
                                                          parser.getFilePath());

  std::vector<std::unique_ptr<sun::ast::MoonScopeAST>> scopes;
  std::set<std::string> supplied;
  // Only identical artifacts share a scope. Explicit listings expose their
  // public names; different embedded revisions remain separate and hidden.
  for (size_t i = 0; i < moonImports.size(); ++i) {
    auto& reader = *readers[i];
    const auto& hash = reader.getLibrary().content_hash();
    supplied.insert(hash);
    scopes.push_back(parser.collectMoonImport(moonImports[i], reader, hash));
  }
  for (size_t i = 0; i < moonImports.size(); ++i) {
    auto& reader = *readers[i];
    for (const auto& key : reader.listModules()) {
      const auto& hash = reader.getMetadata(key)->content_hash();
      if (!supplied.insert(hash).second) {
        continue;
      }
      auto scope = parser.collectMoonImport(MoonImport(moonImports[i].path),
                                            reader, hash);
      scope->setEmbeddedDependency(true);
      scopes.push_back(std::move(scope));
    }
  }
  for (auto& reader : readers) {
    sun::moon_bundling::LibraryCache::instance().addBundle(std::move(reader));
  }
  return scopes;
}

/**
 * Process moon imports: collect stubs, deduplicate, check for collisions
 * with source modules and between moons, then prepend to AST.
 */
static void processMoonImports(BlockExprAST& blockAst, Parser& parser,
                               const std::vector<MoonImport>& moonImports) {
  if (moonImports.empty()) {
    return;
  }

  auto scopes = loadMoonScopes(parser, moonImports);

  // Collect top-level module names from source files
  std::unordered_set<std::string> sourceModuleNames;
  for (const auto& stmt : blockAst.getBody()) {
    if (stmt && stmt->getType() == ASTNodeType::MODULE) {
      auto& mod = static_cast<const ModuleAST&>(*stmt);
      sourceModuleNames.insert(mod.getName());
    }
  }

  // Collect, deduplicate, and check for collisions in one pass
  std::vector<std::unique_ptr<ExprAST>> finalMoonScopeASTs;
  std::unordered_set<std::string> seenHashes;
  std::unordered_map<std::string, std::pair<std::string, std::string>>
      moduleProviders;  // module_name -> (moon_path, content_hash)

  for (auto& stub : scopes) {
    if (!stub) {
      continue;
    }

    auto& moonScope = *stub;
    const std::string& hash = moonScope.getContentHash();
    const std::string& moonPath = moonScope.getMoonPath();

    // Skip duplicate moons (same content hash)
    if (!seenHashes.insert(hash).second) {
      continue;
    }

    // Embedded dependency scopes supply identities, not source module names.
    if (moonScope.isEmbeddedDependency()) {
      finalMoonScopeASTs.push_back(std::move(stub));
      continue;
    }
    // Check for module name collisions
    for (const auto& bodyExpr : moonScope.getBody().getBody()) {
      if (bodyExpr->getType() == ASTNodeType::MODULE) {
        auto& mod = static_cast<ModuleAST&>(*bodyExpr);
        const std::string& modName = mod.getName();

        // Check collision with source file modules
        if (sourceModuleNames.count(modName)) {
          logAndThrowError("Module name collision: module '" + modName +
                           "' from '" + moonPath +
                           "' conflicts with a module in the source files.");
        }

        // Check collision with other moon modules
        auto it = moduleProviders.find(modName);
        if (it != moduleProviders.end()) {
          if (it->second.second != hash) {
            logAndThrowError("Module name collision: module '" + modName +
                             "' is provided by both '" + it->second.first +
                             "' and '" + moonPath +
                             "'. Use module aliasing to resolve.");
          }
        } else {
          moduleProviders[modName] = {moonPath, hash};
        }
      }
    }

    finalMoonScopeASTs.push_back(std::move(stub));
  }

  if (!finalMoonScopeASTs.empty()) {
    blockAst.prependExpressions(std::move(finalMoonScopeASTs));
  }
}

/**
 * Move everything the program declares itself (every top-level node that is
 * not an imported bundle's scope) into one MoonScopeAST for the bundle being
 * built, named by its `$hash$` prefix. Imported scopes stay where they are.
 */
static void wrapOwnBundle(BlockExprAST& blockAst,
                          const std::string& scopeName) {
  auto ownBody = std::make_unique<BlockExprAST>();
  std::vector<std::unique_ptr<ExprAST>> imported;
  for (auto& expr : blockAst.mutableBody()) {
    if (expr->getType() == ASTNodeType::MOON_SCOPE) {
      imported.push_back(std::move(expr));
    } else {
      ownBody->addExpression(std::move(expr));
    }
  }
  blockAst.mutableBody() = std::move(imported);
  blockAst.addExpression(
      sun::ast::MoonScopeAST::forOwnBundle(scopeName, std::move(ownBody)));
}

/**
 * Bind the program's own C externs to the archives the bundle being built
 * carries: an extern whose link name is a key of `renames` is emitted, and
 * recorded in the bundle's metadata, under the prefixed value. Imported
 * bundles' scopes are left alone; their externs were bound when they were
 * built. Link names no rename applies to are collected in `unmapped`.
 */
static void renameOwnExterns(BlockExprAST& block,
                             const std::map<std::string, std::string>& renames,
                             std::vector<std::string>& unmapped) {
  for (auto& stmt : block.mutableBody()) {
    if (!stmt) {
      continue;
    }
    switch (stmt->getType()) {
      case ASTNodeType::MODULE:
        renameOwnExterns(static_cast<ModuleAST&>(*stmt).mutableBody(), renames,
                         unmapped);
        break;
      case ASTNodeType::FUNCTION: {
        auto& func = static_cast<FunctionAST&>(*stmt);
        if (!func.isCExtern()) {
          break;
        }
        auto& proto = func.getProtoMut();
        auto it = renames.find(proto.getLinkName());
        if (it != renames.end()) {
          proto.setLinkName(it->second);
        } else {
          unmapped.push_back(proto.getLinkName());
        }
        break;
      }
      case ASTNodeType::VARIABLE_CREATION: {
        auto& variable = static_cast<sun::ast::VariableCreationAST&>(*stmt);
        if (!variable.isCExtern()) {
          break;
        }
        auto it = renames.find(variable.getLinkName());
        if (it != renames.end()) {
          variable.setLinkName(it->second);
        } else {
          unmapped.push_back(variable.getLinkName());
        }
        break;
      }
      default:
        break;
    }
  }
}

/**
 * Appends the serialized form of every statement the program wrote itself to
 * `out`. Blocks are flattened and imported moon scopes are skipped. Each
 * statement is prefixed with its length so neighbours cannot run together.
 */
static void appendOwnSourceBytes(const ExprAST& node,
                                 sun::serialization::ASTSerializer& serializer,
                                 std::string& out) {
  if (node.getType() == ASTNodeType::MOON_SCOPE) {
    return;
  }
  if (node.getType() == ASTNodeType::BLOCK) {
    sun::ast::forEachChild(node, [&](const ExprAST& child) {
      appendOwnSourceBytes(child, serializer, out);
    });
    return;
  }
  auto bytes = serializer.serialize(node).SerializeAsString();
  out += std::to_string(bytes.size()) + ":" + bytes;
}

/**
 * Returns a hash that identifies a program by its own statements. Imported
 * moon scopes are left out because they already carry their own identity.
 */
static std::string computeOwnSourceHash(const BlockExprAST& blockAst) {
  sun::serialization::ASTSerializer serializer;
  std::string source;
  appendOwnSourceBytes(blockAst, serializer, source);
  return sun::moon_bundling::computeSha256Hex(source);
}

void Driver::analyzeProgram(BlockExprAST& blockAst, Parser& parser) {
  // Lower the lossless parse tree into the core AST before semantic analysis
  sun::parsing::LoweringPass lowering;
  {
    ScopedStage stage("lowering");
    lowering.run(blockAst);
  }

  // Interpolation desugars to std.String / std.HeapAllocator, so those types
  // have to come from somewhere: an imported stdlib.moon, or — when
  // compiling the standard library itself — its own sources.
  if ((parser.usesStringInterpolation() || lowering.usedInterpolation()) &&
      !hasStdlibImport(moonImports_) && !declaresStdlibString(blockAst)) {
    logAndThrowError(
        "String interpolation requires the standard library. Add "
        "'moon \"stdlib.moon\"' to your manifest or use --moon stdlib.moon");
  }

  // Inject AST stubs from moon imports before semantic analysis
  {
    ScopedStage stage("moon imports");
    processMoonImports(blockAst, parser, moonImports_);
  }

  // Bind the program's own C externs to the archives it carries before its
  // scope is wrapped and before metadata is extracted, so the bundle and its
  // importers agree on every symbol name.
  unmappedExternLinkNames_.clear();
  renameOwnExterns(blockAst, externRenames_, unmappedExternLinkNames_);

  // A bundle build compiles its own sources under the bundle's `$hash$`
  // scope, as a sibling of the imported bundles, so its names come out the
  // way importers will compute them.
  if (!ownBundleHash_.empty()) {
    wrapOwnBundle(blockAst, "$" + ownBundleHash_ + "$");
  }

  // Run semantic analysis on the unified AST
  {
    ScopedStage stage("sema");
    // Executables have an artifact identity too; imported trees retain theirs.
    std::string artifactHash = ownBundleHash_;
    if (artifactHash.empty()) {
      artifactHash = computeOwnSourceHash(blockAst);
    }
    analyzer->pipeline().run(blockAst, [&] {
      sun::semantic_analysis::PortableDeclarationKey::assignOriginals(
          blockAst, analysisResults->declarations, artifactHash);
    });
  }
}

}  // namespace sun::driver
