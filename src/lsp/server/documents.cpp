/** Language server documents implementation. */

#include <utility>

#include "lsp/declarations.h"
#include "server.h"

/** Implements the Sun language server. */
namespace sun::lsp {

/** Dispatches documents messages using the current document state. */
bool handleDocuments(
    const std::string& methodName, const llvm::json::Value* id,
    llvm::json::Object* params,
    std::unordered_map<std::string, OpenDocument>& openDocuments) {
  if (methodName == "textDocument/didOpen" && params) {
    llvm::json::Object* textDocument = nullptr;
    if (llvm::json::Value* rawTextDocument = params->get("textDocument")) {
      textDocument = rawTextDocument->getAsObject();
    }
    if (!textDocument) {
      return true;
    }

    std::optional<llvm::StringRef> uri = textDocument->getString("uri");
    std::optional<llvm::StringRef> text = textDocument->getString("text");
    int version =
        static_cast<int>(textDocument->getInteger("version").value_or(0));
    if (!uri || !text) {
      return true;
    }

    OpenDocument document;
    document.uri = uri->str();
    document.path = uriToPath(document.uri);
    document.text = text->str();
    document.version = version;
    openDocuments[document.uri] = document;
    sun::lsp::openSourceOverrides[sun::lsp::normalizePath(document.path)] =
        document.text;
    sun::lsp::refreshEntrypoints();
    clearAnalysisCaches();

    publishDiagnostics(document.uri, analyzeDiagnostics(document), version);
    return true;
  }

  if (methodName == "textDocument/didChange" && params) {
    llvm::json::Object* textDocument = nullptr;
    if (llvm::json::Value* rawTextDocument = params->get("textDocument")) {
      textDocument = rawTextDocument->getAsObject();
    }
    if (!textDocument) {
      return true;
    }

    std::optional<llvm::StringRef> uri = textDocument->getString("uri");
    int version =
        static_cast<int>(textDocument->getInteger("version").value_or(0));
    if (!uri) {
      return true;
    }

    auto documentIter = openDocuments.find(uri->str());
    if (documentIter == openDocuments.end()) {
      return true;
    }

    llvm::json::Array* contentChanges = nullptr;
    if (llvm::json::Value* rawContentChanges = params->get("contentChanges")) {
      contentChanges = rawContentChanges->getAsArray();
    }
    if (!contentChanges || contentChanges->empty()) {
      return true;
    }

    llvm::json::Object* firstChange = (*contentChanges)[0].getAsObject();
    if (!firstChange) {
      return true;
    }

    std::optional<llvm::StringRef> text = firstChange->getString("text");
    if (!text) {
      return true;
    }

    OpenDocument& document = documentIter->second;
    document.text = text->str();
    document.version = version;
    sun::lsp::openSourceOverrides[sun::lsp::normalizePath(document.path)] =
        document.text;
    sun::lsp::refreshEntrypoints();
    clearAnalysisCaches();

    publishDiagnostics(document.uri, analyzeDiagnostics(document), version);
    return true;
  }

  if (methodName == "textDocument/didSave" && params) {
    llvm::json::Object* textDocument = nullptr;
    if (llvm::json::Value* rawTextDocument = params->get("textDocument")) {
      textDocument = rawTextDocument->getAsObject();
    }
    if (!textDocument) {
      return true;
    }

    std::optional<llvm::StringRef> uri = textDocument->getString("uri");
    if (!uri) {
      return true;
    }

    auto documentIter = openDocuments.find(uri->str());
    if (documentIter == openDocuments.end()) {
      return true;
    }

    OpenDocument& document = documentIter->second;
    sun::lsp::refreshEntrypoints();
    // Clear cache to force re-read from disk for manifest-based compilation
    clearAnalysisCaches();
    publishDiagnostics(document.uri, analyzeDiagnostics(document),
                       document.version);
    return true;
  }

  if (methodName == "textDocument/didClose" && params) {
    llvm::json::Object* textDocument = nullptr;
    if (llvm::json::Value* rawTextDocument = params->get("textDocument")) {
      textDocument = rawTextDocument->getAsObject();
    }
    if (!textDocument) {
      return true;
    }

    std::optional<llvm::StringRef> uri = textDocument->getString("uri");
    if (!uri) {
      return true;
    }

    std::string uriString = uri->str();
    sun::lsp::openSourceOverrides.erase(
        sun::lsp::normalizePath(uriToPath(uriString)));
    openDocuments.erase(uriString);
    sun::lsp::refreshEntrypoints();
    clearAnalysisCaches();
    publishDiagnostics(uriString, llvm::json::Array(), 0);
    return true;
  }

  if (methodName == "workspace/didChangeConfiguration" && params) {
    // Configuration arrives under settings.sun. Path variables are applied
    // before entrypoints are re-resolved, since manifests may reference
    // them; a change to either re-analyzes every open document.
    if (const llvm::json::Object* settings = params->getObject("settings")) {
      if (const llvm::json::Object* sunSettings = settings->getObject("sun")) {
        bool variablesChanged = sun::lsp::applyPathVariables(*sunSettings);
        auto configPaths = sun::lsp::readSunConfigs(*sunSettings);
        auto entrypointPaths = sun::lsp::readEntrypoints(*sunSettings);
        if (variablesChanged || configPaths || entrypointPaths) {
          if (configPaths) {
            sun::lsp::configuredSunConfigs = std::move(*configPaths);
          }
          if (entrypointPaths) {
            sun::lsp::explicitEntrypoints = std::move(*entrypointPaths);
          }
          // Re-reads the config files too, so a client can resend the
          // same settings to pick up an edited sun-config.json.
          sun::lsp::refreshEntrypoints();
          // Clear caches since context may have changed
          clearAnalysisCaches();
          // Re-analyze all open documents
          for (auto& [docUri, document] : openDocuments) {
            publishDiagnostics(document.uri, analyzeDiagnostics(document),
                               document.version);
          }
        }
      }
    }
    return true;
  }

  return false;
}

}  // namespace sun::lsp
