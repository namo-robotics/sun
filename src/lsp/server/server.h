/** Shared document state and interfaces between server implementation files. */
#pragma once
#include <llvm/Support/JSON.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "driver/driver.h"
#include "driver/sun_config.h"
#include "lsp/text_positions.h"
/** Implements the Sun language server. */
namespace sun::lsp {

/** The current text and version of a document open in the editor. */
struct OpenDocument {
  std::string uri;
  std::string path;
  std::string text;
  int version = 0;
};

/** The parsed and analyzed state retained for an editor document. */
struct AnalyzedDocument {
  std::string contentHash;
  std::unique_ptr<sun::driver::Driver> driver;
  std::unique_ptr<sun::ast::BlockExprAST> ast;
};

extern std::map<std::string, std::string> openSourceOverrides;
extern std::vector<std::string> configuredSunConfigs;
extern std::vector<std::string> explicitEntrypoints;
extern std::map<std::string, sun::driver::ConfigEntrypoint>
    configEntrypointInfo;
/**
 * Configuration for a single entrypoint file with its manifest data
 */
struct EntrypointConfig {
  std::string entrypointPath;         // Absolute path to entrypoint
  std::vector<std::string> sunFiles;  // All .sun files from manifest
  std::vector<sun::moon_bundling::MoonImport>
      moonImports;                      // Moon imports from manifest
  std::vector<std::string> protoFiles;  // .proto schemas from manifest
  std::set<std::string> coveredFiles;   // Quick lookup of covered files
};

/**
 * Global entrypoint configuration state
 */
class EntrypointManager {
 public:
  /**
   * Set entrypoints from configuration and build file mappings
   */
  void setEntrypoints(const std::vector<std::string>& paths);

  /**
   * Find entrypoint config for a given file path
   * Returns nullptr if file is not covered by any entrypoint
   */
  const EntrypointConfig* findEntrypointForFile(
      const std::string& filePath) const;

  /**
   * Every resolved entrypoint, for workspace-wide requests
   */
  const std::vector<EntrypointConfig>& entrypoints() const;

 private:
  std::vector<std::string> configuredPaths_;
  std::vector<EntrypointConfig> entrypoints_;
  std::unordered_map<std::string, size_t> fileToEntrypoint_;

  /**
   * Parse an entrypoint file and extract manifest information
   */
  std::optional<EntrypointConfig> parseEntrypoint(const std::string& path);
};

extern EntrypointManager entrypointManager;

/** Reloads configured and open manifest entrypoints. */
void refreshEntrypoints();
/** Applies editor-configured compiler path variables. */
bool applyPathVariables(const llvm::json::Object& config);
/** Reads explicit entrypoints from editor configuration. */
std::optional<std::vector<std::string>> readEntrypoints(
    const llvm::json::Object& config);
/** Reads project configuration paths from editor configuration. */
std::optional<std::vector<std::string>> readSunConfigs(
    const llvm::json::Object& config);
/** Inventories expected test binaries and their freshness without analysis. */
llvm::json::Object testSuites();
/** Converts an editor file URI to a filesystem path. */
std::string uriToPath(const std::string& uri);
/** Converts a filesystem path to an editor file URI. */
std::string pathToUri(const std::string& path);
/** Sends a JSON-RPC result to the editor. */
void sendResponse(const llvm::json::Value& id, llvm::json::Value result);
/** Sends a JSON-RPC error to the editor. */
void sendErrorResponse(const llvm::json::Value& id, int code,
                       const std::string& message);
/** Creates a zero-based protocol source range. */
llvm::json::Object makeRange(int startLine, int startCharacter, int endLine,
                             int endCharacter);
/** Computes semantic tokens for document text. */
std::vector<int> computeSemanticTokens(const std::string& source);
/** Analyzes a document in its manifest context and returns diagnostics. */
llvm::json::Array analyzeDiagnostics(const OpenDocument& document);
/** Returns a cached analyzed tree, retaining partial trees after errors. */
const AnalyzedDocument* getAnalyzedDocument(const OpenDocument& document);
/** Clears diagnostics and analyzed trees after document or context changes. */
void clearAnalysisCaches();
/** Publishes diagnostics for the supplied document version. */
void publishDiagnostics(const std::string& uri, llvm::json::Array diagnostics,
                        int version);
/** Reads the next framed JSON-RPC message from standard input. */
std::optional<std::string> readMessageBody();
/** Runs the standard-input language server message loop. */
int runServer();

/** Handles documents messages; returns whether the method was recognized. */
bool handleDocuments(
    const std::string& methodName, const llvm::json::Value* id,
    llvm::json::Object* params,
    std::unordered_map<std::string, OpenDocument>& openDocuments);

/** Handles formatting messages; returns whether the method was recognized. */
bool handleFormatting(
    const std::string& methodName, const llvm::json::Value* id,
    llvm::json::Object* params,
    std::unordered_map<std::string, OpenDocument>& openDocuments);

/** Handles navigation messages; returns whether the method was recognized. */
bool handleNavigation(
    const std::string& methodName, const llvm::json::Value* id,
    llvm::json::Object* params,
    std::unordered_map<std::string, OpenDocument>& openDocuments);

/** Handles test requests messages; returns whether the method was recognized.
 */
bool handleTestRequests(
    const std::string& methodName, const llvm::json::Value* id,
    llvm::json::Object* params,
    std::unordered_map<std::string, OpenDocument>& openDocuments);

}  // namespace sun::lsp
