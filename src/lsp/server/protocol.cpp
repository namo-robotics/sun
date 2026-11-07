/** Language server protocol implementation. */
#include <llvm/Support/FormatVariadic.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

#include "server.h"

/** Implements the Sun language server. */
namespace sun::lsp {

/** Removes the carriage return from a protocol header line. */
static std::string trimCR(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

/** Reports whether text begins with the supplied prefix. */
static bool startsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

/** Decodes percent-escaped bytes in a URI component. */
static std::string percentDecode(std::string_view encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());

  auto hexToInt = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
      return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
      return 10 + (ch - 'A');
    }
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

/** Converts a file URI to a filesystem path. */
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

/**
 * `file://` URI for an absolute path; bytes outside the unreserved set are
 * percent-encoded (the inverse of uriToPath)
 */
std::string pathToUri(const std::string& path) {
  static const char* hex = "0123456789ABCDEF";
  std::string uri = "file://";
  for (unsigned char ch : path) {
    if (std::isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~' ||
        ch == '/') {
      uri.push_back(static_cast<char>(ch));
    } else {
      uri.push_back('%');
      uri.push_back(hex[ch >> 4]);
      uri.push_back(hex[ch & 15]);
    }
  }
  return uri;
}

/** Serializes a JSON value for a language server message. */
static std::string toJsonPayload(llvm::json::Value value) {
  return llvm::formatv("{0}", std::move(value)).str();
}

/** Writes a JSON message with language server framing headers. */
static void writeLSPMessage(llvm::json::Value value) {
  std::string payload = toJsonPayload(std::move(value));
  std::cout << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
  std::cout.flush();
}

/** Sends a successful JSON-RPC response for the supplied request identifier. */
void sendResponse(const llvm::json::Value& id, llvm::json::Value result) {
  llvm::json::Object response;
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  response["result"] = std::move(result);
  writeLSPMessage(std::move(response));
}

/** Sends a JSON-RPC error response with its code and message. */
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

/** Builds a JSON source position for the language server protocol. */
static llvm::json::Object makePosition(int line, int character) {
  llvm::json::Object position;
  position["line"] = std::max(0, line);
  position["character"] = std::max(0, character);
  return position;
}

/** Builds a JSON source range for the language server protocol. */
llvm::json::Object makeRange(int startLine, int startCharacter, int endLine,
                             int endCharacter) {
  llvm::json::Object range;
  range["start"] = makePosition(startLine, startCharacter);
  range["end"] = makePosition(endLine, endCharacter);
  return range;
}

/** Notifies the editor of diagnostics for a specific document version. */
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

/** Reads the next framed language server message from standard input. */
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
      contentLength =
          static_cast<size_t>(std::strtoull(valuePart.c_str(), nullptr, 10));
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

}  // namespace sun::lsp
