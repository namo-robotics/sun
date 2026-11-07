/** Parses and merges compiler inputs and provides compilation entrypoints. */
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ast/manifest_ast.h"
#include "driver/c_bindings.h"
#include "driver/driver.h"
#include "driver/manifest_processor.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/proto_importer.h"
#include "parsing/doc_comments.h"
#include "support/error.h"
#include "support/source_manager.h"
#include "support/stage_timer.h"
#include "support/sun_path.h"

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::ast::ModuleAST;
using sun::codegen::CodegenVisitor;
using sun::driver::ManifestProcessor;
using sun::driver::SunValue;
using sun::driver::VoidValue;
using sun::moon_bundling::MoonImport;
using sun::parsing::Parser;
using sun::semantic_analysis::AnalysisResults;
using sun::semantic_analysis::SemanticAnalyzer;
using sun::support::logAndThrowError;
using sun::support::SourceManager;
using sun::support::SunError;

/** Coordinates compilation, dependency loading, linking, and execution. */
namespace sun::driver {

SunValue Driver::executeString(const std::string& source, int argc, char** argv,
                               const std::string& filePath) {
  std::string effectivePath = filePath;
  if (!filePath.empty()) {
    std::filesystem::path sourcePath = std::filesystem::absolute(filePath);
    baseDir = sourcePath.parent_path().string();
    effectivePath = sourcePath.string();
  } else {
    // Anonymous source (e.g., from executeString in tests)
    effectivePath = SourceManager::instance().addAnonymousSource(source);
  }

  // Register source for error reporting
  if (!filePath.empty()) {
    SourceManager::instance().addSource(effectivePath, source);
  }

  auto parser = Parser::createStringParser(source);
  parser.setBaseDir(baseDir);
  if (!filePath.empty()) {
    parser.setFilePath(filePath);
  } else {
    parser.setFilePath(effectivePath);
  }

  auto blockAst = parser.parseProgram();
  return runPipeline(std::move(blockAst), parser, true, argc, argv);
}

SunValue Driver::executeFile(const std::string& filename, int argc,
                             char** argv) {
  std::filesystem::path filePath = std::filesystem::absolute(filename);
  std::string baseDirPath = filePath.parent_path().string();
  std::string canonical = std::filesystem::canonical(filePath).string();

  std::ifstream file(filename);
  if (!file.is_open()) {
    sun::support::messageStream("error")
        << "Could not open file '" << sun::support::displayPath(filename)
        << "'\n";
    return VoidValue{};
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  // First parse to check for manifest
  auto preParser = Parser::createStringParser(source);
  preParser.setFilePath(canonical);
  auto preAst = preParser.parseProgram();

  std::vector<std::string> sunFiles;
  std::vector<MoonImport> moonImports = moonImports_;
  std::vector<std::string> protoFiles = protoFiles_;

  if (const auto* manifest = ManifestProcessor::findManifest(*preAst)) {
    // Manifest found - collect all dependencies. The module's triple picks
    // which target: blocks apply (the host's for JIT execution).
    auto resolved = ManifestProcessor::process(
        *manifest, baseDirPath, ctx->mainModule->getTargetTriple());
    sunFiles = std::move(resolved.sunFiles);
    moonImports.insert(moonImports.end(), resolved.moonImports.begin(),
                       resolved.moonImports.end());
    protoFiles.insert(protoFiles.end(), resolved.protoFiles.begin(),
                      resolved.protoFiles.end());
    manifestArchivePaths_.clear();
    for (auto& archive : resolved.nativeArchives) {
      manifestArchivePaths_.push_back(std::move(archive.path));
    }
    // test_files are part of the program only when building the test binary
    hasTests_ |= !resolved.testSunFiles.empty();
    if (testHandling_ == TestHandling::Compile) {
      sunFiles.insert(sunFiles.end(), resolved.testSunFiles.begin(),
                      resolved.testSunFiles.end());
    }
  }

  // Add the entrypoint file itself
  sunFiles.insert(sunFiles.begin(), canonical);

  // Use merged compilation
  return executeFiles(sunFiles, moonImports, argc, argv, protoFiles);
}

Parser Driver::prepareStringParser(const std::string& source,
                                   const std::string& filePath) {
  std::string effectivePath = filePath;
  if (!filePath.empty()) {
    std::filesystem::path sourcePath = std::filesystem::absolute(filePath);
    baseDir = sourcePath.parent_path().string();
    effectivePath = sourcePath.string();
  } else {
    // Anonymous source
    effectivePath = SourceManager::instance().addAnonymousSource(source);
  }

  // Register source for error reporting
  SourceManager::instance().addSource(effectivePath, source);

  auto parser = Parser::createStringParser(source);
  parser.setBaseDir(baseDir);
  if (!filePath.empty()) {
    parser.setFilePath(filePath);
  } else {
    parser.setFilePath(effectivePath);
  }
  return parser;
}

void Driver::compileString(const std::string& source,
                           const std::string& filePath) {
  auto parser = prepareStringParser(source, filePath);
  auto blockAst = parser.parseProgram();
  runPipeline(std::move(blockAst), parser, false);
}

void Driver::startAnalysisSession() {
  analyzedTree_.reset();
  analysisResults = std::make_shared<AnalysisResults>();
  analyzer = std::make_unique<SemanticAnalyzer>(analysisResults);
  codegenVisitor = std::make_unique<CodegenVisitor>(*ctx, analysisResults);
}

Driver::AnalyzedProgram Driver::analyzeString(const std::string& source,
                                              const std::string& filePath) {
  startAnalysisSession();
  AnalyzedProgram result;
  result.results = analysisResults;
  try {
    auto parser = prepareStringParser(source, filePath);
    result.ast = parser.parseProgram();
    if (result.ast) {
      sun::parsing::attachDocComments(*result.ast, source);
      analyzeProgram(*result.ast, parser);
    }
  } catch (const SunError& error) {
    result.error = error;
  }
  return result;
}

Driver::AnalyzedProgram Driver::analyzeFiles(
    const std::vector<std::string>& sourceFiles,
    const std::vector<MoonImport>& moonImports,
    const std::vector<std::string>& protoFiles,
    const std::map<std::string, std::string>& sourceOverrides) {
  startAnalysisSession();
  AnalyzedProgram result;
  result.results = analysisResults;
  moonImports_ = moonImports;
  try {
    result.ast = parseAndMergeFiles(sourceFiles, protoFiles, sourceOverrides);
    auto stubParser = Parser::createStringParser("");
    analyzeProgram(*result.ast, stubParser);
  } catch (const SunError& error) {
    result.error = error;
  }
  return result;
}

void Driver::compileFile(const std::string& filename) {
  std::filesystem::path filePath = std::filesystem::absolute(filename);
  std::string baseDirPath = filePath.parent_path().string();
  std::string canonical = std::filesystem::canonical(filePath).string();

  std::ifstream file(filename);
  if (!file.is_open()) {
    sun::support::messageStream("error")
        << "Could not open file '" << sun::support::displayPath(filename)
        << "'\n";
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  // First parse to check for manifest
  auto preParser = Parser::createStringParser(source);
  preParser.setFilePath(canonical);
  auto preAst = preParser.parseProgram();

  std::vector<std::string> sunFiles;
  std::vector<MoonImport> moonImports = moonImports_;
  std::vector<std::string> protoFiles = protoFiles_;

  if (const auto* manifest = ManifestProcessor::findManifest(*preAst)) {
    // Manifest found - collect all dependencies. The module's triple picks
    // which target: blocks apply (the host's for JIT execution).
    auto resolved = ManifestProcessor::process(
        *manifest, baseDirPath, ctx->mainModule->getTargetTriple());
    sunFiles = std::move(resolved.sunFiles);
    moonImports.insert(moonImports.end(), resolved.moonImports.begin(),
                       resolved.moonImports.end());
    protoFiles.insert(protoFiles.end(), resolved.protoFiles.begin(),
                      resolved.protoFiles.end());
    manifestArchivePaths_.clear();
    for (auto& archive : resolved.nativeArchives) {
      manifestArchivePaths_.push_back(std::move(archive.path));
    }
    // test_files are part of the program only when building the test binary
    hasTests_ |= !resolved.testSunFiles.empty();
    if (testHandling_ == TestHandling::Compile) {
      sunFiles.insert(sunFiles.end(), resolved.testSunFiles.begin(),
                      resolved.testSunFiles.end());
    }
  }

  // Add the entrypoint file itself
  sunFiles.insert(sunFiles.begin(), canonical);

  // Use merged compilation
  compileFiles(sunFiles, moonImports, protoFiles);
}

// ---------------------------------------------------------------------------
// Merged-AST compilation: compile multiple source files together
// ---------------------------------------------------------------------------

/**
 * Merge multiple parsed BlockExprASTs into a single unified AST.
 * Same-named modules are merged together.
 */
static std::unique_ptr<BlockExprAST> mergeASTs(
    std::vector<std::unique_ptr<BlockExprAST>>& parsedFiles) {
  std::vector<std::unique_ptr<ExprAST>> mergedBody;

  // Track modules by name so we can merge same-named modules. Emission keeps
  // first-seen order: declaration order across modules must stay stable
  // (a class field of a type from a sibling module resolves in the
  // declaration pre-pass in this order).
  std::unordered_map<std::string, std::vector<std::unique_ptr<ExprAST>>>
      moduleContents;
  std::vector<std::string> moduleOrder;
  std::map<std::string, const ModuleAST*> moduleOrigins;

  // Track non-module statements separately so we can order them after modules
  std::vector<std::unique_ptr<ExprAST>> nonModuleStatements;

  for (size_t fileIdx = 0; fileIdx < parsedFiles.size(); ++fileIdx) {
    auto& fileAST = parsedFiles[fileIdx];
    if (!fileAST) {
      continue;
    }

    auto& body =
        const_cast<std::vector<std::unique_ptr<ExprAST>>&>(fileAST->getBody());

    for (auto& stmt : body) {
      if (!stmt) {
        continue;
      }

      if (stmt->getType() == ASTNodeType::MODULE) {
        // Collect module contents for merging
        auto& mod = static_cast<ModuleAST&>(*stmt);
        const std::string& modName = mod.getName();

        // Move the module body statements to our collection
        auto& modBody = const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
            mod.getBody().getBody());
        if (!moduleContents.count(modName)) {
          moduleOrder.push_back(modName);
          moduleOrigins[modName] = &mod;
          moduleContents[modName];
        } else if (moduleOrigins[modName]->getVisibility() !=
                   mod.getVisibility()) {
          logAndThrowError("all declarations of module '" + modName +
                               "' must agree on its visibility",
                           mod.getLocation());
        }
        for (auto& modStmt : modBody) {
          if (modStmt) {
            moduleContents[modName].push_back(std::move(modStmt));
          }
        }
      } else {
        // Non-module top-level statement - collect separately
        nonModuleStatements.push_back(std::move(stmt));
      }
    }
  }

  // First add merged modules (so they're defined before using statements)
  for (const auto& modName : moduleOrder) {
    auto& contents = moduleContents[modName];
    auto modBody = std::make_unique<BlockExprAST>(std::move(contents),
                                                  sun::ast::BlockKind::Module);
    auto mergedMod = std::make_unique<ModuleAST>(modName, std::move(modBody));
    const auto& origin = *moduleOrigins.at(modName);
    mergedMod->setVisibility(origin.getVisibility());
    mergedMod->setDoc(origin.getDoc());
    if (origin.getNameLocation()) {
      mergedMod->setNameLocation(*origin.getNameLocation());
    }
    mergedMod->setLocation(origin.getLocation());
    mergedMod->setSourceFileId(origin.getSourceFileId());
    mergedBody.push_back(std::move(mergedMod));
  }

  // Then add non-module statements (using, functions, etc.)
  for (auto& stmt : nonModuleStatements) {
    mergedBody.push_back(std::move(stmt));
  }

  return std::make_unique<BlockExprAST>(std::move(mergedBody),
                                        sun::ast::BlockKind::Module);
}

void Driver::parseSynthesizedProtoModules(
    const std::vector<sun::moon_bundling::SynthesizedProtoModule>& protoModules,
    std::vector<std::unique_ptr<BlockExprAST>>& parsedFiles,
    std::vector<std::string>& canonicalPaths) {
  for (const auto& synthesized : protoModules) {
    // Registered under its pseudo-path so diagnostics cite the .proto and
    // show the synthesized source line
    SourceManager::instance().addSource(synthesized.pseudoPath,
                                        synthesized.sunSource);
    if (dumpProtoSun_) {
      llvm::outs() << "// ==== " << synthesized.pseudoPath << " ====\n"
                   << synthesized.sunSource << "\n";
    }
    auto parser = Parser::createStringParser(synthesized.sunSource);
    parser.setFilePath(synthesized.pseudoPath);
    auto blockAst = parser.parseProgram();
    if (!blockAst) {
      throw SunError(
          SunError::Kind::Parse,
          "Failed to parse synthesized module for " + synthesized.pseudoPath);
    }
    canonicalPaths.push_back(synthesized.pseudoPath);
    sun::parsing::attachDocComments(*blockAst, synthesized.sunSource);
    parsedFiles.push_back(std::move(blockAst));
  }
}

std::unique_ptr<BlockExprAST> Driver::parseAndMergeFiles(
    const std::vector<std::string>& sourceFiles,
    const std::vector<std::string>& protoFiles,
    const std::map<std::string, std::string>& sourceOverrides,
    const std::vector<sun::moon_bundling::SynthesizedProtoModule>&
        protoModules) {
  if (sourceFiles.empty()) {
    throw SunError(SunError::Kind::Parse, "No source files specified");
  }

  generateCBindings(
      std::filesystem::absolute(sourceFiles.front()).parent_path().string(),
      ctx->mainModule->getTargetTriple());

  // Parse all source files independently
  std::vector<std::unique_ptr<BlockExprAST>> parsedFiles;
  std::vector<std::string> canonicalPaths;
  parsedFiles.reserve(sourceFiles.size());
  canonicalPaths.reserve(sourceFiles.size());

  for (const auto& filename : sourceFiles) {
    std::filesystem::path filePath = std::filesystem::absolute(filename);
    std::string canonical = std::filesystem::canonical(filePath).string();
    canonicalPaths.push_back(canonical);

    std::string source;
    auto override = sourceOverrides.find(canonical);
    if (override != sourceOverrides.end()) {
      source = override->second;
    } else {
      std::ifstream file(filename);
      if (!file.is_open()) {
        throw SunError(SunError::Kind::Parse,
                       "Could not open file '" + filename + "'");
      }
      std::stringstream buffer;
      buffer << file.rdbuf();
      source = buffer.str();
    }

    // Register source for error reporting
    SourceManager::instance().addSource(canonical, source);

    // Parse (imports always error now - we use merged compilation)
    auto parser = Parser::createStringParser(source);
    parser.setFilePath(canonical);

    auto blockAst = parser.parseProgram();
    if (!blockAst) {
      throw SunError(SunError::Kind::Parse, "Failed to parse " + filename);
    }
    sun::parsing::attachDocComments(*blockAst, source);
    parsedFiles.push_back(std::move(blockAst));
  }

  // Native protobuf import: each .proto becomes ordinary Sun source, parsed
  // like any other file so the merged AST (and everything after it) sees
  // normal code
  const auto importedModules =
      sun::moon_bundling::ProtoImporter::importAll(protoFiles, baseDir);
  parseSynthesizedProtoModules(importedModules, parsedFiles, canonicalPaths);
  parseSynthesizedProtoModules(protoModules, parsedFiles, canonicalPaths);

  // Merge all parsed files into a single AST
  return mergeASTs(parsedFiles);
}

void Driver::compileFiles(
    const std::vector<std::string>& sourceFiles,
    const std::vector<MoonImport>& moonImports,
    const std::vector<std::string>& protoFiles,
    const std::map<std::string, std::string>& sourceOverrides,
    const std::vector<sun::moon_bundling::SynthesizedProtoModule>&
        protoModules) {
  auto mergedAst = parseAndMergeFiles(sourceFiles, protoFiles, sourceOverrides,
                                      protoModules);
  auto stubParser = Parser::createStringParser("");
  moonImports_ = moonImports;
  runPipeline(std::move(mergedAst), stubParser, false);
}

SunValue Driver::executeFiles(const std::vector<std::string>& sourceFiles,
                              const std::vector<MoonImport>& moonImports,
                              int argc, char** argv,
                              const std::vector<std::string>& protoFiles) {
  auto mergedAst = parseAndMergeFiles(sourceFiles, protoFiles, {});
  auto stubParser = Parser::createStringParser("");
  moonImports_ = moonImports;
  return runPipeline(std::move(mergedAst), stubParser, true, argc, argv);
}

}  // namespace sun::driver
