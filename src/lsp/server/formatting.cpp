/** Language server formatting implementation. */

#include <utility>

#include "parsing/formatter.h"
#include "server.h"
#include "support/error.h"

using sun::support::SunError;

/** Implements the Sun language server. */
namespace sun::lsp {

/** Dispatches formatting messages using the current document state. */
bool handleFormatting(
    const std::string& methodName, const llvm::json::Value* id,
    llvm::json::Object* params,
    std::unordered_map<std::string, OpenDocument>& openDocuments) {
  if (methodName == "textDocument/semanticTokens/full" && params) {
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

    const OpenDocument& document = documentIter->second;
    std::vector<int> tokenData = computeSemanticTokens(document.text);

    llvm::json::Array dataArray;
    for (int value : tokenData) {
      dataArray.push_back(value);
    }

    llvm::json::Object result;
    result["data"] = std::move(dataArray);
    sendResponse(*id, std::move(result));
    return true;
  }

  if (methodName == "textDocument/formatting" && params) {
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

    const OpenDocument& document = documentIter->second;
    std::string formatted;
    try {
      formatted = sun::parsing::formatSource(document.text, document.path);
    } catch (const SunError&) {
      // Unparseable code: a null result avoids error popups on every save
      sendResponse(*id, llvm::json::Value(nullptr));
      return true;
    }

    if (formatted == document.text) {
      sendResponse(*id, llvm::json::Array());
      return true;
    }

    // Single whole-document edit: range spans every existing line
    int lastLine = 0;
    int lastCol = 0;
    for (char c : document.text) {
      if (c == '\n') {
        ++lastLine;
        lastCol = 0;
      } else {
        ++lastCol;
      }
    }
    llvm::json::Object edit;
    edit["range"] = makeRange(0, 0, lastLine, lastCol);
    edit["newText"] = formatted;
    llvm::json::Array edits;
    edits.push_back(std::move(edit));
    sendResponse(*id, std::move(edits));
    return true;
  }

  return false;
}

}  // namespace sun::lsp
