/** Initializes the language server and dispatches framed editor messages. */
#include "server.h"

#include <utility>

/** Implements the Sun language server. */
namespace sun::lsp {

/** Reads editor requests and runs the Sun language server message loop. */
int runServer() {
  std::unordered_map<std::string, OpenDocument> openDocuments;
  bool shutdownRequested = false;

  while (true) {
    std::optional<std::string> body = readMessageBody();
    if (!body) {
      break;
    }

    auto parsed = llvm::json::parse(*body);
    if (!parsed) {
      continue;
    }

    llvm::json::Object* message = parsed->getAsObject();
    if (!message) {
      continue;
    }

    std::optional<llvm::StringRef> method = message->getString("method");
    const llvm::json::Value* id = message->get("id");

    if (!method) {
      continue;
    }

    std::string methodName = method->str();

    if (methodName == "initialize") {
      if (!id) {
        continue;
      }

      // Parse initializationOptions. Path variables are applied before
      // entrypoints are resolved, since manifests may reference them.
      if (llvm::json::Value* rawParams = message->get("params")) {
        if (llvm::json::Object* params = rawParams->getAsObject()) {
          if (const llvm::json::Object* initOptions =
                  params->getObject("initializationOptions")) {
            sun::lsp::applyPathVariables(*initOptions);
            if (auto configPaths = sun::lsp::readSunConfigs(*initOptions)) {
              sun::lsp::configuredSunConfigs = std::move(*configPaths);
            }
            if (auto entrypointPaths =
                    sun::lsp::readEntrypoints(*initOptions)) {
              sun::lsp::explicitEntrypoints = std::move(*entrypointPaths);
            }
            sun::lsp::refreshEntrypoints();
          }
        }
      }

      llvm::json::Object textDocumentSync;
      textDocumentSync["openClose"] = true;
      textDocumentSync["change"] = 1;
      textDocumentSync["save"] = true;

      // Semantic tokens legend - token types must match SemanticTokenType enum
      // order
      llvm::json::Array tokenTypes;
      tokenTypes.push_back("module");
      tokenTypes.push_back("type");
      tokenTypes.push_back("class");
      tokenTypes.push_back("enum");
      tokenTypes.push_back("interface");
      tokenTypes.push_back("struct");
      tokenTypes.push_back("typeParameter");
      tokenTypes.push_back("parameter");
      tokenTypes.push_back("variable");
      tokenTypes.push_back("property");
      tokenTypes.push_back("enumMember");
      tokenTypes.push_back("event");
      tokenTypes.push_back("function");
      tokenTypes.push_back("method");
      tokenTypes.push_back("macro");
      tokenTypes.push_back("keyword");
      tokenTypes.push_back("modifier");
      tokenTypes.push_back("comment");
      tokenTypes.push_back("string");
      tokenTypes.push_back("number");
      tokenTypes.push_back("regexp");
      tokenTypes.push_back("operator");
      // Custom: constructors and destructors (init/deinit). Declared by the
      // VSCode extension in contributes.semanticTokenTypes.
      tokenTypes.push_back("lifecycle");

      llvm::json::Array tokenModifiers;
      tokenModifiers.push_back("declaration");
      tokenModifiers.push_back("definition");

      llvm::json::Object legend;
      legend["tokenTypes"] = std::move(tokenTypes);
      legend["tokenModifiers"] = std::move(tokenModifiers);

      llvm::json::Object semanticTokensProvider;
      semanticTokensProvider["legend"] = std::move(legend);
      semanticTokensProvider["full"] = true;

      llvm::json::Object capabilities;
      capabilities["textDocumentSync"] = std::move(textDocumentSync);
      capabilities["semanticTokensProvider"] =
          std::move(semanticTokensProvider);
      capabilities["documentFormattingProvider"] = true;
      capabilities["hoverProvider"] = true;
      capabilities["definitionProvider"] = true;
      capabilities["referencesProvider"] = true;
      llvm::json::Object renameProvider;
      renameProvider["prepareProvider"] = true;
      capabilities["renameProvider"] = std::move(renameProvider);
      // Non-standard: the client's test explorer probes for these before
      // sending sun/tests and sun/workspaceTests requests.
      llvm::json::Object experimental;
      experimental["sunTests"] = true;
      experimental["sunWorkspaceTests"] = true;
      experimental["sunTestSuites"] = true;
      capabilities["experimental"] = std::move(experimental);

      llvm::json::Object serverInfo;
      serverInfo["name"] = "sun-lsp";
      serverInfo["version"] = "0.1.0";

      llvm::json::Object result;
      result["capabilities"] = std::move(capabilities);
      result["serverInfo"] = std::move(serverInfo);

      sendResponse(*id, std::move(result));
      continue;
    }

    if (methodName == "shutdown") {
      if (!id) {
        continue;
      }
      shutdownRequested = true;
      sendResponse(*id, llvm::json::Value(nullptr));
      continue;
    }

    if (methodName == "exit") {
      return shutdownRequested ? 0 : 1;
    }

    llvm::json::Object* params = nullptr;
    if (llvm::json::Value* rawParams = message->get("params")) {
      params = rawParams->getAsObject();
    }

    if (handleDocuments(methodName, id, params, openDocuments)) {
      continue;
    }

    if (handleFormatting(methodName, id, params, openDocuments)) {
      continue;
    }

    if (handleNavigation(methodName, id, params, openDocuments)) {
      continue;
    }

    if (handleTestRequests(methodName, id, params, openDocuments)) {
      continue;
    }

    if (id) {
      sendErrorResponse(*id, -32601,
                        std::string("Method not implemented: ") + methodName);
    }
  }

  return 0;
}

}  // namespace sun::lsp
