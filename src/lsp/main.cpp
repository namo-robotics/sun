#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>

#include "driver.h"
#include "error.h"

namespace {

struct OpenDocument {
  std::string uri;
  std::string path;
  std::string text;
  int version = 0;
};

std::string trimCR(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

bool startsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

std::string percentDecode(std::string_view encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());

  auto hexToInt = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
    return -1;
  };

  for (size_t index = 0; index < encoded.size(); ++index) {
    if (encoded[index] == '%' && index + 2 < encoded.size()) {
      int high = hexToInt(encoded[index + 1]);
      int low = hexToInt(encoded[index + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        continue;
      }
    }
    decoded.push_back(encoded[index]);
  }

  return decoded;
}

std::string uriToPath(const std::string& uri) {
  if (!startsWith(uri, "file://")) {
    return uri;
  }

  std::string pathPart = uri.substr(7);
  pathPart = percentDecode(pathPart);

  if (!pathPart.empty() && pathPart[0] != '/') {
    pathPart = "/" + pathPart;
  }

  return pathPart;
}

std::string toJsonPayload(llvm::json::Value value) {
  return llvm::formatv("{0}", std::move(value)).str();
}

void writeLSPMessage(llvm::json::Value value) {
  std::string payload = toJsonPayload(std::move(value));
  std::cout << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
  std::cout.flush();
}

void sendResponse(const llvm::json::Value& id, llvm::json::Value result) {
  llvm::json::Object response;
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  response["result"] = std::move(result);
  writeLSPMessage(std::move(response));
}

void sendErrorResponse(const llvm::json::Value& id, int code,
                       const std::string& message) {
  llvm::json::Object error;
  error["code"] = code;
  error["message"] = message;

  llvm::json::Object response;
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  response["error"] = std::move(error);
  writeLSPMessage(std::move(response));
}

llvm::json::Object makePosition(int line, int character) {
  llvm::json::Object position;
  position["line"] = std::max(0, line);
  position["character"] = std::max(0, character);
  return position;
}

llvm::json::Object makeRange(int startLine, int startCharacter, int endLine,
                             int endCharacter) {
  llvm::json::Object range;
  range["start"] = makePosition(startLine, startCharacter);
  range["end"] = makePosition(endLine, endCharacter);
  return range;
}

llvm::json::Array analyzeDiagnostics(const OpenDocument& document) {
  llvm::json::Array diagnostics;

  try {
    auto driver = Driver::createForAOT("sun-lsp");
    driver->compileString(document.text, document.path);
  } catch (const SunError& error) {
    int startLine = 0;
    int startCharacter = 0;
    int endLine = 0;
    int endCharacter = 1;

    if (error.getLocation()) {
      const Position& location = *error.getLocation();
      startLine = std::max(0, location.line - 1);
      startCharacter = std::max(0, location.column - 1);
      endLine = startLine;
      endCharacter = startCharacter + 1;
    }

    llvm::json::Object diagnostic;
    diagnostic["range"] =
        makeRange(startLine, startCharacter, endLine, endCharacter);
    diagnostic["severity"] = 1;
    diagnostic["source"] = "sun";
    diagnostic["message"] = error.getMessage();
    diagnostics.push_back(std::move(diagnostic));
  } catch (const std::exception& error) {
    llvm::json::Object diagnostic;
    diagnostic["range"] = makeRange(0, 0, 0, 1);
    diagnostic["severity"] = 1;
    diagnostic["source"] = "sun";
    diagnostic["message"] = std::string("Internal error: ") + error.what();
    diagnostics.push_back(std::move(diagnostic));
  }

  return diagnostics;
}

void publishDiagnostics(const std::string& uri, llvm::json::Array diagnostics,
                        int version) {
  llvm::json::Object params;
  params["uri"] = uri;
  params["diagnostics"] = std::move(diagnostics);
  params["version"] = version;

  llvm::json::Object notification;
  notification["jsonrpc"] = "2.0";
  notification["method"] = "textDocument/publishDiagnostics";
  notification["params"] = std::move(params);
  writeLSPMessage(std::move(notification));
}

std::optional<std::string> readMessageBody() {
  std::string line;
  size_t contentLength = 0;

  while (std::getline(std::cin, line)) {
    line = trimCR(line);
    if (line.empty()) {
      break;
    }

    constexpr std::string_view headerName = "Content-Length:";
    if (line.rfind(headerName, 0) == 0) {
      std::string valuePart = line.substr(headerName.size());
      while (!valuePart.empty() && std::isspace(valuePart.front())) {
        valuePart.erase(valuePart.begin());
      }
      contentLength = static_cast<size_t>(std::strtoull(valuePart.c_str(), nullptr, 10));
    }
  }

  if (contentLength == 0) {
    return std::nullopt;
  }

  std::string body(contentLength, '\0');
  std::cin.read(body.data(), static_cast<std::streamsize>(contentLength));
  if (std::cin.gcount() != static_cast<std::streamsize>(contentLength)) {
    return std::nullopt;
  }

  return body;
}

}  // namespace

int main() {
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
      if (!id) continue;

      llvm::json::Object textDocumentSync;
      textDocumentSync["openClose"] = true;
      textDocumentSync["change"] = 1;

      llvm::json::Object capabilities;
      capabilities["textDocumentSync"] = std::move(textDocumentSync);

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
      if (!id) continue;
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

    if (methodName == "textDocument/didOpen" && params) {
      llvm::json::Object* textDocument = nullptr;
      if (llvm::json::Value* rawTextDocument = params->get("textDocument")) {
        textDocument = rawTextDocument->getAsObject();
      }
      if (!textDocument) continue;

      std::optional<llvm::StringRef> uri = textDocument->getString("uri");
      std::optional<llvm::StringRef> text = textDocument->getString("text");
      int version = static_cast<int>(textDocument->getInteger("version").value_or(0));
      if (!uri || !text) continue;

      OpenDocument document;
      document.uri = uri->str();
      document.path = uriToPath(document.uri);
      document.text = text->str();
      document.version = version;
      openDocuments[document.uri] = document;

      publishDiagnostics(document.uri, analyzeDiagnostics(document), version);
      continue;
    }

    if (methodName == "textDocument/didChange" && params) {
      llvm::json::Object* textDocument = nullptr;
      if (llvm::json::Value* rawTextDocument = params->get("textDocument")) {
        textDocument = rawTextDocument->getAsObject();
      }
      if (!textDocument) continue;

      std::optional<llvm::StringRef> uri = textDocument->getString("uri");
      int version = static_cast<int>(textDocument->getInteger("version").value_or(0));
      if (!uri) continue;

      auto documentIter = openDocuments.find(uri->str());
      if (documentIter == openDocuments.end()) continue;

      llvm::json::Array* contentChanges = nullptr;
      if (llvm::json::Value* rawContentChanges = params->get("contentChanges")) {
        contentChanges = rawContentChanges->getAsArray();
      }
      if (!contentChanges || contentChanges->empty()) continue;

      llvm::json::Object* firstChange = (*contentChanges)[0].getAsObject();
      if (!firstChange) continue;

      std::optional<llvm::StringRef> text = firstChange->getString("text");
      if (!text) continue;

      OpenDocument& document = documentIter->second;
      document.text = text->str();
      document.version = version;

      publishDiagnostics(document.uri, analyzeDiagnostics(document), version);
      continue;
    }

    if (methodName == "textDocument/didClose" && params) {
      llvm::json::Object* textDocument = nullptr;
      if (llvm::json::Value* rawTextDocument = params->get("textDocument")) {
        textDocument = rawTextDocument->getAsObject();
      }
      if (!textDocument) continue;

      std::optional<llvm::StringRef> uri = textDocument->getString("uri");
      if (!uri) continue;

      std::string uriString = uri->str();
      openDocuments.erase(uriString);
      publishDiagnostics(uriString, llvm::json::Array(), 0);
      continue;
    }

    if (id) {
      sendErrorResponse(*id, -32601,
                        std::string("Method not implemented: ") + methodName);
    }
  }

  return 0;
}
