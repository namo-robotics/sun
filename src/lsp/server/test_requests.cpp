/** Language server test requests implementation. */

#include <exception>
#include <fstream>
#include <sstream>
#include <utility>

#include "lsp/declarations.h"
#include "lsp/name_ranges.h"
#include "lsp/tests.h"
#include "server.h"

using sun::driver::Driver;

/** Implements the Sun language server. */
namespace sun::lsp {

/** Dispatches test requests messages using the current document state. */
bool handleTestRequests(
    const std::string& methodName, const llvm::json::Value* id,
    llvm::json::Object* params,
    std::unordered_map<std::string, OpenDocument>& openDocuments) {
  if (methodName == "sun/tests" && params) {
    if (!id) {
      return true;
    }

    llvm::json::Object* textDocument = nullptr;
    if (llvm::json::Value* rawTextDocument = params->get("textDocument")) {
      textDocument = rawTextDocument->getAsObject();
    }
    if (!textDocument) {
      sendErrorResponse(*id, -32602, "Missing textDocument parameter");
      return true;
    }
    std::optional<llvm::StringRef> uri = textDocument->getString("uri");
    if (!uri) {
      sendErrorResponse(*id, -32602, "Missing textDocument.uri");
      return true;
    }
    auto documentIter = openDocuments.find(uri->str());
    if (documentIter == openDocuments.end()) {
      sendErrorResponse(*id, -32602, "Document not open");
      return true;
    }

    sun::lsp::refreshEntrypoints();
    clearAnalysisCaches();
    const OpenDocument& document = documentIter->second;
    const sun::lsp::EntrypointConfig* entrypoint =
        sun::lsp::entrypointManager.findEntrypointForFile(document.path);
    if (!entrypoint) {
      llvm::json::Object result;
      result["entrypoint"] = nullptr;
      result["tests"] = llvm::json::Array();
      sendResponse(*id, std::move(result));
      return true;
    }
    llvm::json::Array tests;
    if (const AnalyzedDocument* analyzed = getAnalyzedDocument(document)) {
      try {
        for (const auto& item : sun::lsp::collectTests(
                 *analyzed->ast, document.path, document.text)) {
          llvm::json::Object test;
          test["id"] = item.id;
          test["label"] = item.label;
          test["range"] =
              makeRange(item.location.start.line, item.location.start.character,
                        item.location.end.line, item.location.end.character);
          tests.push_back(std::move(test));
        }
      } catch (const std::exception&) {
        tests = llvm::json::Array();
      }
    }

    llvm::json::Object result;
    result["entrypoint"] = entrypoint->entrypointPath;
    result["tests"] = std::move(tests);
    sendResponse(*id, std::move(result));
    return true;
  }

  // Workspace-wide test discovery for the editor's test explorer: every
  // test in every configured entrypoint, plus what the client needs to
  // run them — the entrypoint to hand `sun test`, the configured test
  // binary (from a sun-config entrypoints entry, when there is one), and
  // the sources whose timestamps say whether that binary is fresh.
  if (methodName == "sun/testSuites") {
    if (id) {
      sendResponse(*id, sun::lsp::testSuites());
    }
    return true;
  }

  if (methodName == "sun/workspaceTests") {
    if (!id) {
      return true;
    }

    sun::lsp::refreshEntrypoints();
    clearAnalysisCaches();

    // Unsaved edits in open documents stand in for their files, the same
    // way per-document analysis treats the requested buffer.
    std::map<std::string, std::string> overrides;
    for (const auto& [docUri, document] : openDocuments) {
      overrides[sun::lsp::normalizePath(document.path)] = document.text;
    }

    llvm::json::Array entrypointsJson;
    for (const auto& entrypoint : sun::lsp::entrypointManager.entrypoints()) {
      std::vector<sun::lsp::TestSpan> spans;
      try {
        auto driver = Driver::createForAOT("sun-lsp");
        auto program =
            driver->analyzeFiles(entrypoint.sunFiles, entrypoint.moonImports,
                                 entrypoint.protoFiles, overrides);
        if (program.ast) {
          spans = sun::lsp::collectTestSpans(*program.ast);
        }
      } catch (const std::exception&) {
        spans.clear();
      }

      // Group tests by file, narrowing each span to its name token with
      // that file's text: the open buffer when there is one, the file on
      // disk otherwise.
      std::map<std::string, llvm::json::Array> testsByFile;
      std::map<std::string, std::string> fileTexts;
      for (const auto& span : spans) {
        auto text = fileTexts.find(span.filePath);
        if (text == fileTexts.end()) {
          std::string content;
          auto override = overrides.find(span.filePath);
          if (override != overrides.end()) {
            content = override->second;
          } else {
            std::ifstream in(span.filePath);
            std::stringstream buffer;
            buffer << in.rdbuf();
            content = buffer.str();
          }
          text = fileTexts.emplace(span.filePath, std::move(content)).first;
        }
        auto location = sun::lsp::makeSymbolLocation(
            span.filePath,
            sun::lsp::nameRange(span.span, span.label, text->second),
            text->second);
        llvm::json::Object test;
        test["id"] = span.id;
        test["label"] = span.label;
        test["range"] = makeRange(location.start.line, location.start.character,
                                  location.end.line, location.end.character);
        testsByFile[span.filePath].push_back(std::move(test));
      }

      llvm::json::Array files;
      for (auto& [filePath, fileTests] : testsByFile) {
        llvm::json::Object fileJson;
        fileJson["uri"] = pathToUri(filePath);
        fileJson["tests"] = std::move(fileTests);
        files.push_back(std::move(fileJson));
      }

      llvm::json::Object entryJson;
      entryJson["entrypoint"] = entrypoint.entrypointPath;
      auto info = sun::lsp::configEntrypointInfo.find(
          sun::lsp::normalizePath(entrypoint.entrypointPath));
      if (info != sun::lsp::configEntrypointInfo.end() &&
          !info->second.testBinaryName.empty()) {
        entryJson["test_binary"] = info->second.testBinaryName;
      } else {
        entryJson["test_binary"] = nullptr;
      }
      llvm::json::Array sources;
      for (const auto& file : entrypoint.sunFiles) {
        sources.push_back(file);
      }
      entryJson["sources"] = std::move(sources);
      entryJson["files"] = std::move(files);
      entrypointsJson.push_back(std::move(entryJson));
    }

    llvm::json::Object result;
    result["entrypoints"] = std::move(entrypointsJson);
    sendResponse(*id, std::move(result));
    return true;
  }

  return false;
}

}  // namespace sun::lsp
