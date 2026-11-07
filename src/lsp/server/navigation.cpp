/** Language server navigation implementation. */

#include <exception>
#include <utility>

#include "lsp/definition.h"
#include "lsp/hover.h"
#include "lsp/name_ranges.h"
#include "lsp/references.h"
#include "lsp/rename.h"
#include "server.h"

using sun::lsp::SymbolLocation;

/** Implements the Sun language server. */
namespace sun::lsp {

/** Dispatches navigation messages using the current document state. */
bool handleNavigation(
    const std::string& methodName, const llvm::json::Value* id,
    llvm::json::Object* params,
    std::unordered_map<std::string, OpenDocument>& openDocuments) {
  if (methodName == "textDocument/hover" && params) {
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

    llvm::json::Object* position = nullptr;
    if (llvm::json::Value* rawPosition = params->get("position")) {
      position = rawPosition->getAsObject();
    }
    if (!position) {
      sendErrorResponse(*id, -32602, "Missing position parameter");
      return true;
    }
    int line = static_cast<int>(position->getInteger("line").value_or(0));
    int character =
        static_cast<int>(position->getInteger("character").value_or(0));

    auto documentIter = openDocuments.find(uri->str());
    if (documentIter == openDocuments.end()) {
      sendErrorResponse(*id, -32602, "Document not open");
      return true;
    }

    const OpenDocument& document = documentIter->second;
    const AnalyzedDocument* analyzed = getAnalyzedDocument(document);
    std::optional<sun::lsp::Hover> hover;
    if (analyzed) {
      int offset =
          sun::lsp::byteOffsetFromLspPosition(document.text, line, character);
      try {
        hover = sun::lsp::computeHover(*analyzed->ast, document.path,
                                       document.text, offset);
      } catch (const std::exception&) {
        hover.reset();
      }
    }
    if (!hover) {
      sendResponse(*id, llvm::json::Value(nullptr));
      return true;
    }

    auto start =
        sun::lsp::lspPositionFromByteOffset(document.text, hover->range.offset);
    auto end = sun::lsp::lspPositionFromByteOffset(
        document.text, hover->range.endOffset.value_or(hover->range.offset));

    std::string value = "```sun\n" + hover->code + "\n```";
    if (!hover->documentation.empty()) {
      value += "\n\n---\n\n" + hover->documentation;
    }
    llvm::json::Object contents;
    contents["kind"] = "markdown";
    contents["value"] = std::move(value);

    llvm::json::Object result;
    result["contents"] = std::move(contents);
    result["range"] =
        makeRange(start.line, start.character, end.line, end.character);
    sendResponse(*id, std::move(result));
    return true;
  }

  if (methodName == "textDocument/definition" && params) {
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

    llvm::json::Object* position = nullptr;
    if (llvm::json::Value* rawPosition = params->get("position")) {
      position = rawPosition->getAsObject();
    }
    if (!position) {
      sendErrorResponse(*id, -32602, "Missing position parameter");
      return true;
    }
    int line = static_cast<int>(position->getInteger("line").value_or(0));
    int character =
        static_cast<int>(position->getInteger("character").value_or(0));

    auto documentIter = openDocuments.find(uri->str());
    if (documentIter == openDocuments.end()) {
      sendErrorResponse(*id, -32602, "Document not open");
      return true;
    }

    const OpenDocument& document = documentIter->second;
    const AnalyzedDocument* analyzed = getAnalyzedDocument(document);
    std::optional<SymbolLocation> definition;
    if (analyzed) {
      int offset =
          sun::lsp::byteOffsetFromLspPosition(document.text, line, character);
      try {
        definition = sun::lsp::computeDefinition(*analyzed->ast, document.path,
                                                 document.text, offset);
      } catch (const std::exception&) {
        definition.reset();
      }
    }
    if (!definition) {
      sendResponse(*id, llvm::json::Value(nullptr));
      return true;
    }

    llvm::json::Object location;
    location["uri"] = pathToUri(definition->filePath);
    location["range"] =
        makeRange(definition->start.line, definition->start.character,
                  definition->end.line, definition->end.character);
    sendResponse(*id, std::move(location));
    return true;
  }

  if (methodName == "textDocument/references" && params) {
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

    llvm::json::Object* position = nullptr;
    if (llvm::json::Value* rawPosition = params->get("position")) {
      position = rawPosition->getAsObject();
    }
    if (!position) {
      sendErrorResponse(*id, -32602, "Missing position parameter");
      return true;
    }
    int line = static_cast<int>(position->getInteger("line").value_or(0));
    int character =
        static_cast<int>(position->getInteger("character").value_or(0));

    bool includeDeclaration = false;
    if (llvm::json::Object* context = params->getObject("context")) {
      includeDeclaration =
          context->getBoolean("includeDeclaration").value_or(false);
    }

    auto documentIter = openDocuments.find(uri->str());
    if (documentIter == openDocuments.end()) {
      sendErrorResponse(*id, -32602, "Document not open");
      return true;
    }

    const OpenDocument& document = documentIter->second;
    const AnalyzedDocument* analyzed = getAnalyzedDocument(document);
    std::vector<SymbolLocation> references;
    if (analyzed) {
      int offset =
          sun::lsp::byteOffsetFromLspPosition(document.text, line, character);
      try {
        references = sun::lsp::computeReferences(*analyzed->ast, document.path,
                                                 document.text, offset,
                                                 includeDeclaration);
      } catch (const std::exception&) {
        references.clear();
      }
    }

    llvm::json::Array locations;
    for (const auto& reference : references) {
      llvm::json::Object location;
      location["uri"] = pathToUri(reference.filePath);
      location["range"] =
          makeRange(reference.start.line, reference.start.character,
                    reference.end.line, reference.end.character);
      locations.push_back(std::move(location));
    }
    sendResponse(*id, std::move(locations));
    return true;
  }

  if ((methodName == "textDocument/prepareRename" ||
       methodName == "textDocument/rename") &&
      params) {
    if (!id) {
      return true;
    }
    bool isRename = methodName == "textDocument/rename";

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

    llvm::json::Object* position = nullptr;
    if (llvm::json::Value* rawPosition = params->get("position")) {
      position = rawPosition->getAsObject();
    }
    if (!position) {
      sendErrorResponse(*id, -32602, "Missing position parameter");
      return true;
    }
    int line = static_cast<int>(position->getInteger("line").value_or(0));
    int character =
        static_cast<int>(position->getInteger("character").value_or(0));

    std::string newName;
    if (isRename) {
      std::optional<llvm::StringRef> rawName = params->getString("newName");
      if (!rawName) {
        sendErrorResponse(*id, -32602, "Missing newName parameter");
        return true;
      }
      newName = rawName->str();
      std::string problem = sun::lsp::checkNewName(newName);
      if (!problem.empty()) {
        sendErrorResponse(*id, -32602, problem);
        return true;
      }
    }

    auto documentIter = openDocuments.find(uri->str());
    if (documentIter == openDocuments.end()) {
      sendErrorResponse(*id, -32602, "Document not open");
      return true;
    }

    const OpenDocument& document = documentIter->second;
    const AnalyzedDocument* analyzed = getAnalyzedDocument(document);
    std::optional<sun::lsp::Rename> rename;
    int offset = 0;
    if (analyzed) {
      offset =
          sun::lsp::byteOffsetFromLspPosition(document.text, line, character);
      try {
        rename = sun::lsp::computeRename(*analyzed->ast, document.path,
                                         document.text, offset);
      } catch (const std::exception&) {
        rename.reset();
      }
    }
    if (!rename) {
      sendResponse(*id, llvm::json::Value(nullptr));
      return true;
    }

    if (!isRename) {
      // The identifier under the cursor, with its current name to edit
      std::optional<SymbolLocation> site =
          sun::lsp::siteAt(*rename, document.path, offset);
      if (!site || !rename->refusal.empty()) {
        sendResponse(*id, llvm::json::Value(nullptr));
        return true;
      }
      llvm::json::Object result;
      result["range"] = makeRange(site->start.line, site->start.character,
                                  site->end.line, site->end.character);
      result["placeholder"] = rename->name;
      sendResponse(*id, std::move(result));
      return true;
    }

    if (!rename->refusal.empty()) {
      sendErrorResponse(*id, -32803, rename->refusal);
      return true;
    }

    // One text edit per site, grouped by file; the sites arrive sorted by
    // file so each file's edits are contiguous
    llvm::json::Object changes;
    std::string currentUri;
    llvm::json::Array edits;
    auto flush = [&]() {
      if (!currentUri.empty()) {
        changes[currentUri] = std::move(edits);
      }
      edits = llvm::json::Array();
    };
    for (const auto& site : rename->sites) {
      std::string siteUri = pathToUri(site.filePath);
      if (siteUri != currentUri) {
        flush();
        currentUri = siteUri;
      }
      llvm::json::Object edit;
      edit["range"] = makeRange(site.start.line, site.start.character,
                                site.end.line, site.end.character);
      edit["newText"] = newName;
      edits.push_back(std::move(edit));
    }
    flush();
    llvm::json::Object workspaceEdit;
    workspaceEdit["changes"] = std::move(changes);
    sendResponse(*id, std::move(workspaceEdit));
    return true;
  }

  // Custom request for the editor's test explorer: every test_function in
  // one document, with the dotted name the runner's --test-filter matches
  // and the range of the name token. An unanalyzable document answers with
  // an empty list, not an error.
  return false;
}

}  // namespace sun::lsp
