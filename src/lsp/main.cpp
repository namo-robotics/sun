#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/SHA256.h>

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "driver.h"
#include "error.h"
#include "lexer.h"

// =============================================================================
// Diagnostics Cache
// =============================================================================
// Cache compilation diagnostics by content hash to avoid recompiling unchanged
// files. This dramatically improves LSP responsiveness.

struct CachedDiagnostics {
  std::string contentHash;
  llvm::json::Array diagnostics;
};

class DiagnosticsCache {
 public:
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

  bool has(const std::string& hash) const {
    return cache_.find(hash) != cache_.end();
  }

  const llvm::json::Array& get(const std::string& hash) const {
    return cache_.at(hash);
  }

  void put(const std::string& hash, llvm::json::Array diagnostics) {
    // Simple LRU: evict oldest if too many entries
    if (cache_.size() >= maxEntries_) {
      cache_.erase(cache_.begin());
    }
    cache_[hash] = std::move(diagnostics);
  }

  void invalidate(const std::string& hash) { cache_.erase(hash); }

  void clear() { cache_.clear(); }

  size_t size() const { return cache_.size(); }

 private:
  std::unordered_map<std::string, llvm::json::Array> cache_;
  static constexpr size_t maxEntries_ = 100;
};

static DiagnosticsCache diagnosticsCache;

// LSP semantic token type indices (must match legend in initialize response)
namespace LSPTokenType {
constexpr int Module = 0;
constexpr int Type = 1;
constexpr int Class = 2;
constexpr int Enum = 3;
constexpr int Interface = 4;
constexpr int Struct = 5;
constexpr int TypeParameter = 6;
constexpr int Parameter = 7;
constexpr int Variable = 8;
constexpr int Property = 9;
constexpr int EnumMember = 10;
constexpr int Event = 11;
constexpr int Function = 12;
constexpr int Method = 13;
constexpr int Macro = 14;
constexpr int Keyword = 15;
constexpr int Modifier = 16;
constexpr int Comment = 17;
constexpr int String = 18;
constexpr int Number = 19;
constexpr int Regexp = 20;
constexpr int Operator = 21;
}  // namespace LSPTokenType

// Map TokenKind to LSP semantic token type index (-1 = skip)
int tokenKindToLSPType(TokenKind kind) {
  switch (kind) {
    case TokenKind::IF:
    case TokenKind::ELSE:
    case TokenKind::FOR:
    case TokenKind::WHILE:
    case TokenKind::BREAK:
    case TokenKind::CONTINUE:
    case TokenKind::RETURN:
    case TokenKind::TRY:
    case TokenKind::CATCH:
    case TokenKind::THROW:
    case TokenKind::IMPORT:
    case TokenKind::USING:
    case TokenKind::MODULE:
    case TokenKind::NAMESPACE:
    case TokenKind::MATCH:
    case TokenKind::EXTERN:
    case TokenKind::DECLARE:
    case TokenKind::THIS:
    case TokenKind::VAR:
    case TokenKind::FUNCTION:
    case TokenKind::LAMBDA:
    case TokenKind::CLASS:
    case TokenKind::INTERFACE:
    case TokenKind::IMPLEMENTS:
    case TokenKind::ENUM:
    case TokenKind::TRUE_LITERAL:
    case TokenKind::FALSE_LITERAL:
    case TokenKind::NULL_LITERAL:
      return LSPTokenType::Keyword;

    case TokenKind::TYPE_I8:
    case TokenKind::TYPE_I16:
    case TokenKind::TYPE_I32:
    case TokenKind::TYPE_I64:
    case TokenKind::TYPE_U8:
    case TokenKind::TYPE_U16:
    case TokenKind::TYPE_U32:
    case TokenKind::TYPE_U64:
    case TokenKind::TYPE_F32:
    case TokenKind::TYPE_F64:
    case TokenKind::TYPE_BOOL:
    case TokenKind::TYPE_VOID:
    case TokenKind::ARRAY:
      return LSPTokenType::Type;

    case TokenKind::REF:
    case TokenKind::RAW_PTR:
    case TokenKind::STATIC_PTR:
    case TokenKind::PTR:
      return LSPTokenType::Modifier;

    case TokenKind::INTEGER:
    case TokenKind::FLOAT:
      return LSPTokenType::Number;

    case TokenKind::STRING:
      return LSPTokenType::String;

    case TokenKind::PLUS:
    case TokenKind::MINUS:
    case TokenKind::STAR:
    case TokenKind::SLASH:
    case TokenKind::EQUAL:
    case TokenKind::EQUAL_EQUAL:
    case TokenKind::NOT_EQUAL:
    case TokenKind::LESS_EQUAL:
    case TokenKind::GREATER_EQUAL:
    case TokenKind::FAT_ARROW:
      return LSPTokenType::Operator;

    case TokenKind::INTRINSIC_IDENTIFIER:
      return LSPTokenType::Function;

    default:
      return -1;
  }
}

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

// Compute semantic tokens for a document
// Returns encoded token data as per LSP spec: [deltaLine, deltaStartChar,
// length, tokenType, tokenModifiers]...
std::vector<int> computeSemanticTokens(const std::string& source) {
  std::vector<int> data;
  std::istringstream stream(source);
  Lexer lexer(stream);

  // Track context for classifying identifiers as type vs value
  int angleBracketDepth = 0;
  TokenKind prevKind = TokenKind::TOK_EOF;
  bool afterClass = false;
  bool afterInterface = false;
  bool afterImplements = false;
  bool afterFunction = false;
  bool afterColon = false;
  bool afterArrow = false;
  bool afterDot = false;  // For method calls: obj.method()

  struct SemanticToken {
    int line;       // 0-indexed
    int startChar;  // 0-indexed
    int length;
    int tokenType;
    int modifiers = 0;
  };
  std::vector<SemanticToken> tokens;

  // Track last identifier to reclassify on ( or < lookahead
  struct {
    bool valid = false;
    int line;
    int col;
    int length;
    std::string text;
  } lastIdent;

  while (true) {
    Token tok = lexer.getNextToken();
    if (tok.kind == TokenKind::TOK_EOF) break;

    int line = tok.start.line - 1;   // Convert to 0-indexed
    int col = tok.start.column - 1;  // Convert to 0-indexed
    int length = static_cast<int>(tok.text.size());
    if (length == 0) {
      length = tok.end.column - tok.start.column;
      if (length <= 0) length = 1;
    }

    // Try the simple mapping first
    int tokenType = tokenKindToLSPType(tok.kind);

    // Check if previous identifier should be reclassified as function/method
    if (lastIdent.valid) {
      if (tok.kind == TokenKind::PAREN_OPEN || tok.kind == TokenKind::LESS) {
        // Previous identifier is a function/method call
        // Find and update it in tokens
        for (auto& t : tokens) {
          if (t.line == lastIdent.line && t.startChar == lastIdent.col) {
            t.tokenType = LSPTokenType::Function;
            break;
          }
        }
        if (tok.kind == TokenKind::LESS) {
          angleBracketDepth++;  // Generic call: create<T>()
        }
      }
      lastIdent.valid = false;
    }

    // Handle context tracking for keywords that affect following tokens
    if (tok.kind == TokenKind::CLASS)
      afterClass = true;
    else if (tok.kind == TokenKind::INTERFACE)
      afterInterface = true;
    else if (tok.kind == TokenKind::IMPLEMENTS)
      afterImplements = true;
    else if (tok.kind == TokenKind::FUNCTION)
      afterFunction = true;
    else if (tok.kind == TokenKind::COLON)
      afterColon = true;
    else if (tok.kind == TokenKind::ARROW)
      afterArrow = true;
    else if (tok.kind == TokenKind::DOT)
      afterDot = true;
    else if (tok.kind == TokenKind::LESS) {
      // Could be generic or comparison - check context
      if (prevKind == TokenKind::IDENTIFIER || afterClass || afterInterface ||
          afterFunction)
        angleBracketDepth++;
    } else if (tok.kind == TokenKind::GREATER && angleBracketDepth > 0)
      angleBracketDepth--;
    else if (tok.kind == TokenKind::COMMA) {
      afterColon = false;
      afterArrow = false;
    } else if (tok.kind == TokenKind::PAREN_OPEN) {
      // Reset declaration context but keep angle brackets for generic calls
      afterClass = afterInterface = afterImplements = afterFunction = false;
      afterColon = afterArrow = afterDot = false;
    } else if (tok.kind == TokenKind::PAREN_CLOSE ||
               tok.kind == TokenKind::BRACE_OPEN ||
               tok.kind == TokenKind::SEMI_COLON) {
      afterClass = afterInterface = afterImplements = afterFunction = false;
      afterColon = afterArrow = afterDot = false;
      if (tok.kind == TokenKind::BRACE_OPEN ||
          tok.kind == TokenKind::SEMI_COLON)
        angleBracketDepth = 0;
    } else if (tok.kind == TokenKind::BRACE_CLOSE)
      angleBracketDepth = 0;

    // Identifiers need context-based classification
    else if (tok.kind == TokenKind::IDENTIFIER) {
      bool isTypeContext = afterColon || afterArrow || afterImplements ||
                           afterClass || afterInterface ||
                           angleBracketDepth > 0;

      if (afterClass) {
        tokenType = LSPTokenType::Class;
        afterClass = false;
      } else if (afterInterface) {
        tokenType = LSPTokenType::Interface;
        afterInterface = false;
      } else if (afterFunction) {
        tokenType = LSPTokenType::Function;
        afterFunction = false;
        // Track this - might be followed by < for generics
        lastIdent = {true, line, col, length, tok.text};
      } else if (isTypeContext) {
        if (angleBracketDepth > 0 && tok.text.size() <= 2 &&
            std::isupper(tok.text[0]))
          tokenType = LSPTokenType::TypeParameter;
        else
          tokenType = LSPTokenType::Type;
      } else if (afterDot) {
        // Member access - could be property or method
        // Default to property, will be reclassified to method if followed by (
        tokenType = LSPTokenType::Property;
        lastIdent = {true, line, col, length, tok.text};
        afterDot = false;
      } else {
        tokenType = LSPTokenType::Variable;
        // Track for potential function call: foo() or foo<T>()
        lastIdent = {true, line, col, length, tok.text};
      }
      afterColon = afterArrow = afterImplements = false;
    }

    // Special case for string length (includes quotes)
    if (tok.kind == TokenKind::STRING)
      length = tok.end.column - tok.start.column;

    if (tokenType >= 0 && length > 0) {
      tokens.push_back({line, col, length, tokenType, 0});
    }

    prevKind = tok.kind;
  }

  // Encode as delta format
  int prevLine = 0;
  int prevCol = 0;
  for (const auto& tok : tokens) {
    int deltaLine = tok.line - prevLine;
    int deltaCol = (deltaLine == 0) ? (tok.startChar - prevCol) : tok.startChar;
    data.push_back(deltaLine);
    data.push_back(deltaCol);
    data.push_back(tok.length);
    data.push_back(tok.tokenType);
    data.push_back(tok.modifiers);
    prevLine = tok.line;
    prevCol = tok.startChar;
  }

  return data;
}

llvm::json::Array analyzeDiagnostics(const OpenDocument& document) {
  // Compute content hash for caching
  std::string contentHash = DiagnosticsCache::computeHash(document.text);

  // Check cache first - if unchanged, return cached diagnostics
  if (diagnosticsCache.has(contentHash)) {
    // Return a copy of cached diagnostics
    llvm::json::Array result;
    for (const auto& diag : diagnosticsCache.get(contentHash)) {
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
  diagnosticsCache.put(contentHash, std::move(cacheEntry));

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
      int version =
          static_cast<int>(textDocument->getInteger("version").value_or(0));
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
      int version =
          static_cast<int>(textDocument->getInteger("version").value_or(0));
      if (!uri) continue;

      auto documentIter = openDocuments.find(uri->str());
      if (documentIter == openDocuments.end()) continue;

      llvm::json::Array* contentChanges = nullptr;
      if (llvm::json::Value* rawContentChanges =
              params->get("contentChanges")) {
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

    if (methodName == "textDocument/semanticTokens/full" && params) {
      if (!id) continue;

      llvm::json::Object* textDocument = nullptr;
      if (llvm::json::Value* rawTextDocument = params->get("textDocument")) {
        textDocument = rawTextDocument->getAsObject();
      }
      if (!textDocument) {
        sendErrorResponse(*id, -32602, "Missing textDocument parameter");
        continue;
      }

      std::optional<llvm::StringRef> uri = textDocument->getString("uri");
      if (!uri) {
        sendErrorResponse(*id, -32602, "Missing textDocument.uri");
        continue;
      }

      auto documentIter = openDocuments.find(uri->str());
      if (documentIter == openDocuments.end()) {
        sendErrorResponse(*id, -32602, "Document not open");
        continue;
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
      continue;
    }

    if (id) {
      sendErrorResponse(*id, -32601,
                        std::string("Method not implemented: ") + methodName);
    }
  }

  return 0;
}
