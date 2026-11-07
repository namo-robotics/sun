/** Language server analysis implementation. */
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <utility>

#include "lsp/declarations.h"
#include "server.h"
#include "support/error.h"
#include "support/source_manager.h"

using sun::driver::Driver;
using sun::support::SunError;

/** Implements the Sun language server. */
namespace sun::lsp {

/** Stores analysis results so unchanged documents can reuse diagnostics. */
class DiagnosticsCache {
 public:
  /** Computes the content key used to reuse document diagnostics. */
  static std::string computeHash(const std::string& content) {
    llvm::SHA256 sha;
    sha.update(llvm::StringRef(content));
    auto hashBytes = sha.final();
    std::string hash;
    hash.reserve(64);
    for (uint8_t b : hashBytes) {
      char hex[3];
      snprintf(hex, sizeof(hex), "%02x", b);
      hash += hex;
    }
    return hash;
  }

  /** Reports whether diagnostics have been cached for the supplied content
   * hash. */
  bool has(const std::string& hash) const {
    return cache_.find(hash) != cache_.end();
  }

  /** Returns cached diagnostics for a document content hash when available. */
  const llvm::json::Array& get(const std::string& hash) const {
    return cache_.at(hash);
  }

  /** Caches diagnostics under the document content hash. */
  void put(const std::string& hash, llvm::json::Array diagnostics) {
    // Bound memory use; eviction order is deliberately unspecified.
    if (cache_.size() >= maxEntries_) {
      cache_.erase(cache_.begin());
    }
    cache_[hash] = std::move(diagnostics);
  }

  /** Removes all cached document diagnostics. */
  void clear() { cache_.clear(); }

 private:
  std::unordered_map<std::string, llvm::json::Array> cache_;
  static constexpr size_t maxEntries_ = 100;
};

static DiagnosticsCache diagnosticsCache;

/**
 * Add one diagnostic for a message and the place it points at. A location in
 * this document is marked where it happened; one in another file (a manifest
 * sibling, or a library body instantiated from a bundle) is shown at the top
 * of this document, naming where it came from.
 */
static void appendDiagnostic(
    llvm::json::Array& diagnostics, const OpenDocument& document,
    const std::string& message,
    const std::optional<sun::support::Position>& location, int severity,
    const std::string& knownSourceLine = "") {
  bool isFromCurrentFile = true;
  if (location && location->filePath) {
    isFromCurrentFile =
        normalizePath(*location->filePath) == normalizePath(document.path);
  }

  llvm::json::Object diagnostic;
  diagnostic["severity"] = severity;
  diagnostic["source"] = "sun";

  if (!isFromCurrentFile) {
    diagnostic["range"] = makeRange(0, 0, 0, 1);
    diagnostic["message"] = *location->filePath + ":" +
                            std::to_string(location->line) + ": " + message;
    diagnostics.push_back(std::move(diagnostic));
    return;
  }

  int startLine = 0;
  int startCharacter = 0;
  int endLine = 0;
  int endCharacter = 1;

  if (location) {
    startLine = std::max(0, location->line - 1);
    startCharacter = std::max(0, location->column - 1);

    // Use end position if available, otherwise calculate from source
    if (location->hasEnd()) {
      endLine = std::max(0, *location->endLine - 1);
      endCharacter = std::max(0, *location->endColumn - 1);
    } else {
      endLine = startLine;
      // Span the word at the column, so the squiggle covers the whole name
      std::string sourceLine = knownSourceLine;
      if (sourceLine.empty()) {
        sourceLine = sun::support::SourceManager::instance().getLine(*location);
      }
      int tokenLength = 1;
      if (!sourceLine.empty() &&
          startCharacter < static_cast<int>(sourceLine.size())) {
        size_t pos = static_cast<size_t>(startCharacter);
        while (pos < sourceLine.size() &&
               (std::isalnum(static_cast<unsigned char>(sourceLine[pos])) ||
                sourceLine[pos] == '_')) {
          pos++;
        }
        tokenLength = std::max(1, static_cast<int>(pos) - startCharacter);
      }
      endCharacter = startCharacter + tokenLength;
    }
  }

  diagnostic["range"] =
      makeRange(startLine, startCharacter, endLine, endCharacter);
  diagnostic["message"] = message;
  diagnostics.push_back(std::move(diagnostic));
}

/** Analyzes the current document text and collects compiler diagnostics. */
llvm::json::Array analyzeDiagnostics(const OpenDocument& document) {
  // *.inja.sun files are Sun templates: their splices cannot parse, so
  // diagnosing them would only report the template syntax as errors. They
  // still get Sun highlighting; they just stay quiet.
  if (llvm::StringRef(document.path).ends_with(".inja.sun")) {
    return llvm::json::Array();
  }

  // Compute content hash for caching
  // Include entrypoint info in hash to invalidate when context changes
  std::string cacheKey =
      document.path + ":" + DiagnosticsCache::computeHash(document.text);

  // Check cache first - if unchanged, return cached diagnostics
  if (diagnosticsCache.has(cacheKey)) {
    // Return a copy of cached diagnostics
    llvm::json::Array result;
    for (const auto& diag : diagnosticsCache.get(cacheKey)) {
      if (const auto* obj = diag.getAsObject()) {
        llvm::json::Object copy;
        for (const auto& kv : *obj) {
          copy[kv.first] = kv.second;
        }
        result.push_back(std::move(copy));
      }
    }
    return result;
  }

  llvm::json::Array diagnostics;

  try {
    auto driver = Driver::createForAOT("sun-lsp");

    // Check if file is covered by an entrypoint manifest
    const EntrypointConfig* entrypoint =
        entrypointManager.findEntrypointForFile(document.path);

    if (entrypoint && !entrypoint->sunFiles.empty()) {
      driver->compileFiles(entrypoint->sunFiles, entrypoint->moonImports,
                           entrypoint->protoFiles, openSourceOverrides);
    } else {
      // Single-file compilation (existing behavior)
      driver->compileString(document.text, document.path);
    }
  } catch (const SunError& error) {
    // An editor shows no label of its own, so the borrow checker's — the one
    // label that names the pass rather than repeating "error" — leads the
    // message
    std::string message = error.getMessage();
    if (error.getKind() == sun::support::SunError::Kind::Borrow) {
      message = error.getLabel() + ": " + message;
    }
    appendDiagnostic(diagnostics, document, message, error.getLocation(),
                     /*severity=*/1, error.getSourceLine());
    // A pass that finds several problems at once (the borrow checker) lists
    // the rest on the error, each with its own place in the source
    for (const auto& related : error.getRelated()) {
      int severity =
          related.level == sun::support::RelatedDiagnostic::Level::Error ? 1
                                                                         : 3;
      appendDiagnostic(diagnostics, document, related.message, related.location,
                       severity);
    }
  } catch (const std::exception& error) {
    llvm::json::Object diagnostic;
    diagnostic["range"] = makeRange(0, 0, 0, 1);
    diagnostic["severity"] = 1;
    diagnostic["source"] = "sun";
    diagnostic["message"] = std::string("Internal error: ") + error.what();
    diagnostics.push_back(std::move(diagnostic));
  }

  // Cache the result (make a copy for the cache)
  llvm::json::Array cacheEntry;
  for (const auto& diag : diagnostics) {
    if (const auto* obj = diag.getAsObject()) {
      llvm::json::Object copy;
      for (const auto& kv : *obj) {
        copy[kv.first] = kv.second;
      }
      cacheEntry.push_back(std::move(copy));
    }
  }
  diagnosticsCache.put(cacheKey, std::move(cacheEntry));

  return diagnostics;
}

// =============================================================================
// Analyzed documents (for hover)
// =============================================================================
// A document's analyzed tree, built on the first hover request for a given
// text and kept until the text changes. The driver stays alive with the tree
// so the types it owns remain valid; members are declared so the tree is
// destroyed first.

static std::unordered_map<std::string, AnalyzedDocument> analyzedDocuments;

/**
 * Analyzed tree for the document's current text, or null when it cannot be
 * parsed. Analysis errors keep the partial tree: everything typed before the
 * error still answers hover requests.
 */
const AnalyzedDocument* getAnalyzedDocument(const OpenDocument& document) {
  std::string hash = DiagnosticsCache::computeHash(document.text);
  auto cached = analyzedDocuments.find(document.uri);
  if (cached != analyzedDocuments.end() && cached->second.contentHash == hash) {
    return &cached->second;
  }
  analyzedDocuments.erase(document.uri);

  try {
    auto driver = Driver::createForAOT("sun-lsp");
    sun::driver::Driver::AnalyzedProgram program;
    const EntrypointConfig* entrypoint =
        entrypointManager.findEntrypointForFile(document.path);
    if (entrypoint && !entrypoint->sunFiles.empty()) {
      // Manifest context; the open buffer stands in for its file on disk
      program =
          driver->analyzeFiles(entrypoint->sunFiles, entrypoint->moonImports,
                               entrypoint->protoFiles, openSourceOverrides);
    } else {
      program = driver->analyzeString(document.text, document.path);
    }
    if (!program.ast) {
      return nullptr;
    }

    AnalyzedDocument analyzed;
    analyzed.contentHash = std::move(hash);
    analyzed.driver = std::move(driver);
    analyzed.ast = std::move(program.ast);
    auto inserted =
        analyzedDocuments.emplace(document.uri, std::move(analyzed));
    return &inserted.first->second;
  } catch (const std::exception&) {
    return nullptr;
  }
}

/** Invalidates results whose source text or configuration changed. */
void clearAnalysisCaches() {
  diagnosticsCache.clear();
  analyzedDocuments.clear();
}

}  // namespace sun::lsp
