#pragma once

#include <array>
#include <cctype>
#include <cstdlib>  // for strtod
#include <format>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "nfa.h"

enum class TokenKind {
  TOK_EOF,
  COMMENT,  // // comment (skipped during lexing)
  DECLARE,  // declare keyword for explicit generic instantiation
  DEF,
  EXTERN,
  VAR,
  IMPORT,         // import keyword for module system
  MODULE,         // module keyword (preferred over namespace)
  NAMESPACE,      // namespace keyword (deprecated, use module)
  USING,          // using keyword for namespace imports
  CLASS,          // class keyword
  INTERFACE,      // interface keyword
  ENUM,           // enum keyword
  IMPLEMENTS,     // implements keyword
  THIS,           // this keyword
  NULL_LITERAL,   // null keyword
  TRUE_LITERAL,   // true keyword
  FALSE_LITERAL,  // false keyword
  IF,
  MATCH,  // match keyword for pattern matching
  ELSE,
  FOR,
  WHILE,
  BREAK,     // break keyword for loop control
  CONTINUE,  // continue keyword for loop control
  RETURN,
  FUNCTION,  // function keyword
  LAMBDA,    // lambda keyword
  TRY,       // try keyword for error handling
  CATCH,     // catch keyword for exception handling
  THROW,     // throw keyword for throwing exceptions
  // Type keywords (must come before IDENTIFIER for priority)
  STATIC_PTR,            // static_ptr (pointer to immortal static data)
  PTR,                   // ptr (unique/owning pointer with RAII)
  RAW_PTR,               // raw_ptr (non-owning pointer for C interop)
  REF,                   // ref (reference type)
  ARRAY,                 // array (fixed-size array type)
  ARROW,                 // ->
  FAT_ARROW,             // =>
  UNDERSCORE,            // _
  TYPE_I8,               // i8
  TYPE_I16,              // i16
  TYPE_I32,              // i32
  TYPE_I64,              // i64
  TYPE_U8,               // u8
  TYPE_U16,              // u16
  TYPE_U32,              // u32
  TYPE_U64,              // u64
  TYPE_F32,              // f32
  TYPE_F64,              // f64
  TYPE_BOOL,             // bool
  TYPE_VOID,             // void
  STRING,                // string literal "..."
  BRACE_OPEN,            // {
  BRACE_CLOSE,           // }
  BRACKET_OPEN,          // [
  BRACKET_CLOSE,         // ]
  PLUS,                  // +
  MINUS,                 // -
  STAR,                  // *
  SLASH,                 // /
  LESS,                  // <
  LESS_EQUAL,            // <=
  GREATER,               // >
  GREATER_EQUAL,         // >=
  EQUAL,                 // =
  EQUAL_EQUAL,           // ==
  NOT_EQUAL,             // !=
  PAREN_OPEN,            // (
  PAREN_CLOSE,           // )
  COMMA,                 // ,
  SEMI_COLON,            // ;
  DOUBLE_COLON,          // ::
  COLON,                 // :
  ELLIPSIS,              // ...
  DOT,                   // .
  INTEGER,               // integer literal: 0, 1, 42, etc.
  FLOAT,                 // floating-point literal: 3.14, 1e5, 2.0, etc.
  INTRINSIC_IDENTIFIER,  // _name - intrinsic identifiers
  IDENTIFIER,
  UNKNOWN,
  COUNT
};

static const std::map<TokenKind, std::string> tokenRegexes = {
    {TokenKind::COMMENT, "//[^\n]*"},  // Line comments (skipped)
    {TokenKind::DECLARE, "declare"},
    {TokenKind::DEF, "def"},
    {TokenKind::EXTERN, "extern"},
    {TokenKind::VAR, "var"},
    {TokenKind::IMPORT, "import"},
    {TokenKind::MODULE, "module"},
    {TokenKind::NAMESPACE, "namespace"},
    {TokenKind::USING, "using"},
    {TokenKind::CLASS, "class"},
    {TokenKind::INTERFACE, "interface"},
    {TokenKind::ENUM, "enum"},
    {TokenKind::IMPLEMENTS, "implements"},
    {TokenKind::THIS, "this"},
    {TokenKind::NULL_LITERAL, "null"},
    {TokenKind::TRUE_LITERAL, "true"},
    {TokenKind::FALSE_LITERAL, "false"},
    {TokenKind::IF, "if"},
    {TokenKind::MATCH, "match"},
    {TokenKind::ELSE, "else"},
    {TokenKind::FOR, "for"},
    {TokenKind::WHILE, "while"},
    {TokenKind::BREAK, "break"},
    {TokenKind::CONTINUE, "continue"},
    {TokenKind::FUNCTION, "function"},
    {TokenKind::LAMBDA, "lambda"},
    {TokenKind::TRY, "try"},
    {TokenKind::CATCH, "catch"},
    {TokenKind::THROW, "throw"},
    {TokenKind::FAT_ARROW, "=>"},
    {TokenKind::ARROW, "->"},
    {TokenKind::UNDERSCORE, "_"},
    {TokenKind::STATIC_PTR, "static_ptr"},  // Must come before ptr
    {TokenKind::RAW_PTR, "raw_ptr"},        // Must come before ptr
    {TokenKind::PTR, "ptr"},
    {TokenKind::REF, "ref"},
    {TokenKind::ARRAY, "array"},
    {TokenKind::BRACE_OPEN, "\\{"},
    {TokenKind::BRACE_CLOSE, "\\}"},
    {TokenKind::BRACKET_OPEN, "\\["},
    {TokenKind::BRACKET_CLOSE, "\\]"},
    {TokenKind::RETURN, "return"},
    // Type keywords (must come before IDENTIFIER)
    {TokenKind::TYPE_I8, "i8"},
    {TokenKind::TYPE_I16, "i16"},
    {TokenKind::TYPE_I32, "i32"},
    {TokenKind::TYPE_I64, "i64"},
    {TokenKind::TYPE_U8, "u8"},
    {TokenKind::TYPE_U16, "u16"},
    {TokenKind::TYPE_U32, "u32"},
    {TokenKind::TYPE_U64, "u64"},
    {TokenKind::TYPE_F32, "f32"},
    {TokenKind::TYPE_F64, "f64"},
    {TokenKind::TYPE_BOOL, "bool"},
    {TokenKind::TYPE_VOID, "void"},
    {TokenKind::STRING, "\"[^\"]*\""},
    {TokenKind::INTRINSIC_IDENTIFIER, "_[a-zA-Z0-9_]+"},
    {TokenKind::IDENTIFIER, "[a-zA-Z][a-zA-Z0-9_]*"},
    // FLOAT must come before INTEGER so longer match wins (3.0 matches FLOAT,
    // not INTEGER)
    {TokenKind::FLOAT,
     "(0|[1-9][0-9]*)(\\.[0-9]+([eE][+-]?[0-9]+)?|[eE][+-]?[0-9]+)"},
    {TokenKind::INTEGER, "0|[1-9][0-9]*"},
    {TokenKind::PLUS, "\\+"},
    {TokenKind::MINUS, "-"},
    {TokenKind::STAR, "\\*"},
    {TokenKind::SLASH, "/"},
    {TokenKind::LESS_EQUAL, "<="},
    {TokenKind::LESS, "<"},
    {TokenKind::GREATER_EQUAL, ">="},
    {TokenKind::GREATER, ">"},
    {TokenKind::EQUAL_EQUAL, "=="},
    {TokenKind::NOT_EQUAL, "!="},
    {TokenKind::EQUAL, "="},
    {TokenKind::PAREN_OPEN, "\\("},
    {TokenKind::PAREN_CLOSE, "\\)"},
    {TokenKind::COMMA, ","},
    {TokenKind::SEMI_COLON, ";"},
    {TokenKind::DOUBLE_COLON, "::"},
    {TokenKind::COLON, ":"},
    {TokenKind::ELLIPSIS, "\\.\\.\\."},
    {TokenKind::DOT, "\\."}};

// Lookup table for simple token metadata (text and precedence)
struct TokenInfo {
  std::string_view text;
  int precedence = -1;
};

inline const std::map<TokenKind, TokenInfo>& getTokenInfo() {
  static const std::map<TokenKind, TokenInfo> tokenInfo = {
      {TokenKind::TOK_EOF, {""}},
      {TokenKind::DEF, {"def"}},
      {TokenKind::EXTERN, {"extern"}},
      {TokenKind::VAR, {"var"}},
      {TokenKind::IMPORT, {"import"}},
      {TokenKind::MODULE, {"module"}},
      {TokenKind::NAMESPACE, {"namespace"}},
      {TokenKind::USING, {"using"}},
      {TokenKind::CLASS, {"class"}},
      {TokenKind::INTERFACE, {"interface"}},
      {TokenKind::ENUM, {"enum"}},
      {TokenKind::IMPLEMENTS, {"implements"}},
      {TokenKind::THIS, {"this"}},
      {TokenKind::NULL_LITERAL, {"null"}},
      {TokenKind::TRUE_LITERAL, {"true"}},
      {TokenKind::FALSE_LITERAL, {"false"}},
      {TokenKind::IF, {"if"}},
      {TokenKind::MATCH, {"match"}},
      {TokenKind::ELSE, {"else"}},
      {TokenKind::FOR, {"for"}},
      {TokenKind::WHILE, {"while"}},
      {TokenKind::BREAK, {"break"}},
      {TokenKind::CONTINUE, {"continue"}},
      {TokenKind::RETURN, {"return"}},
      {TokenKind::FUNCTION, {"function"}},
      {TokenKind::LAMBDA, {"lambda"}},
      {TokenKind::TRY, {"try"}},
      {TokenKind::CATCH, {"catch"}},
      {TokenKind::THROW, {"throw"}},
      {TokenKind::STATIC_PTR, {"static_ptr"}},
      {TokenKind::PTR, {"ptr"}},
      {TokenKind::RAW_PTR, {"raw_ptr"}},
      {TokenKind::REF, {"ref"}},
      {TokenKind::ARROW, {"->"}},
      {TokenKind::FAT_ARROW, {"=>"}},
      {TokenKind::UNDERSCORE, {"_"}},
      {TokenKind::TYPE_I8, {"i8"}},
      {TokenKind::TYPE_I16, {"i16"}},
      {TokenKind::TYPE_I32, {"i32"}},
      {TokenKind::TYPE_I64, {"i64"}},
      {TokenKind::TYPE_U8, {"u8"}},
      {TokenKind::TYPE_U16, {"u16"}},
      {TokenKind::TYPE_U32, {"u32"}},
      {TokenKind::TYPE_U64, {"u64"}},
      {TokenKind::TYPE_F32, {"f32"}},
      {TokenKind::TYPE_F64, {"f64"}},
      {TokenKind::TYPE_BOOL, {"bool"}},
      {TokenKind::TYPE_VOID, {"void"}},
      {TokenKind::BRACE_OPEN, {"{"}},
      {TokenKind::BRACE_CLOSE, {"}"}},
      {TokenKind::BRACKET_OPEN, {"["}},
      {TokenKind::BRACKET_CLOSE, {"]"}},
      {TokenKind::PAREN_OPEN, {"("}},
      {TokenKind::PAREN_CLOSE, {")"}},
      {TokenKind::COMMA, {","}},
      {TokenKind::SEMI_COLON, {";"}},
      {TokenKind::DOUBLE_COLON, {"::"}},
      {TokenKind::COLON, {":"}},
      {TokenKind::ELLIPSIS, {"..."}},
      {TokenKind::DOT, {"."}},
      {TokenKind::EQUAL, {"="}},
      {TokenKind::UNKNOWN, {""}},
      // Operators with precedence
      {TokenKind::PLUS, {"+", 20}},
      {TokenKind::MINUS, {"-", 20}},
      {TokenKind::STAR, {"*", 40}},
      {TokenKind::SLASH, {"/", 40}},
      {TokenKind::LESS, {"<", 10}},
      {TokenKind::LESS_EQUAL, {"<=", 10}},
      {TokenKind::GREATER, {">", 10}},
      {TokenKind::GREATER_EQUAL, {">=", 10}},
      {TokenKind::EQUAL_EQUAL, {"==", 10}},
      {TokenKind::NOT_EQUAL, {"!=", 10}},
  };
  return tokenInfo;
}

struct Token {
  TokenKind kind;
  std::variant<std::monostate,  // No value: EOF, keywords, operators, UNKNOWN
               std::string,     // IDENTIFIER, STRING
               int64_t,         // INTEGER
               double           // FLOAT
               >
      value;
  Position start;
  Position end;
  std::string text;
  int precedence = -1;

  // Generic factory for simple tokens (uses lookup table)
  static Token make(TokenKind k, Position s, Position e) {
    const auto& info = getTokenInfo();
    if (auto it = info.find(k); it != info.end()) {
      return {k,
              std::monostate{},
              s,
              e,
              std::string(it->second.text),
              it->second.precedence};
    }
    return {k, std::monostate{}, s, e, "", -1};
  }

  // Factories for value-carrying tokens
  static Token eof(Position pos) { return make(TokenKind::TOK_EOF, pos, pos); }

  static Token identifier(std::string id, Position s, Position e) {
    return {TokenKind::IDENTIFIER, id, s, e, std::move(id)};
  }

  static Token intrinsicIdentifier(std::string id, Position s, Position e) {
    return {TokenKind::INTRINSIC_IDENTIFIER, id, s, e, std::move(id)};
  }

  static Token integer(int64_t num, Position s, Position e, std::string txt) {
    return {TokenKind::INTEGER, num, s, e, std::move(txt)};
  }

  static Token floatNum(double num, Position s, Position e, std::string txt) {
    return {TokenKind::FLOAT, num, s, e, std::move(txt)};
  }

  static Token stringLiteral(std::string str, Position s, Position e) {
    return {TokenKind::STRING, std::move(str), s, e, ""};
  }

  bool isEof() const { return kind == TokenKind::TOK_EOF; }

  std::optional<std::string> getIdentifier() const {
    if (kind == TokenKind::IDENTIFIER ||
        kind == TokenKind::INTRINSIC_IDENTIFIER)
      return std::get<std::string>(value);
    return std::nullopt;
  }

  bool isIntrinsicIdentifier() const {
    return kind == TokenKind::INTRINSIC_IDENTIFIER;
  }

  std::optional<int64_t> getInteger() const {
    if (kind == TokenKind::INTEGER) return std::get<int64_t>(value);
    return std::nullopt;
  }

  std::optional<double> getFloat() const {
    if (kind == TokenKind::FLOAT) return std::get<double>(value);
    return std::nullopt;
  }

  std::optional<std::string> getString() const {
    if (kind == TokenKind::STRING) return std::get<std::string>(value);
    return std::nullopt;
  }
};

class Lexer {
 private:
  std::istream* input;
  char currentChar = ' ';
  Position currentPos{1, 1, 0};
  RegexParser regexParser;

  std::string buffer;

  char advance() {
    if (currentPos.offset >= (static_cast<int>(buffer.size()))) {
      currentChar = input->get();
      if (currentChar != EOF) {
        buffer += currentChar;
      }
    } else {
      currentChar = buffer[currentPos.offset];
    }

    if (currentChar != EOF) {
      ++currentPos.offset;
      if (currentChar == '\n') {
        ++currentPos.line;
        currentPos.column = 1;
      } else {
        ++currentPos.column;
      }
    }

    return currentChar;
  }

  char peek() const {
    if (currentPos.offset >= buffer.size()) {
      return input->peek();
    } else {
      return buffer[currentPos.offset];
    }
  }

  // void skip_whitespace()
  // {
  //     while (std::isspace(peek()))
  //     {
  //         advance();
  //     }
  // }

 public:
  void setPosition(const Position& pos) {
    currentPos = pos;
    currentChar = buffer[pos.offset];
  }

  Position getPosition() const { return currentPos; }

  // Extract source text substring from buffer (for storing generic method
  // source)
  std::string getSourceText(int startOffset, int endOffset) const {
    if (startOffset < 0 || endOffset > static_cast<int>(buffer.size()) ||
        startOffset >= endOffset) {
      return "";
    }
    return buffer.substr(startOffset, endOffset - startOffset);
  }

 private:
  // Build the full regex string once (expensive string operations)
  static const std::string& getStaticFullRegex() {
    static std::string fullRegex = []() {
      std::string regex = "[ \n\t\r]*(";
      int N = static_cast<int>(TokenKind::COUNT);
      std::vector<std::string> regexes;
      for (auto i = 0; i < N; ++i) {
        TokenKind kind = static_cast<TokenKind>(i);
        if (tokenRegexes.find(kind) == tokenRegexes.end()) continue;
        std::string regexStr = tokenRegexes.at(kind);
        regexes.push_back(std::format("(?<{}>{})", i, regexStr));
      }
      for (size_t i = 0; i < regexes.size(); ++i) {
        regex += regexes[i];
        if (i < regexes.size() - 1) {
          regex += "|";
        }
      }
      regex += ")";
      return regex;
    }();
    return fullRegex;
  }

  // Build and cache the NFA once - this is the expensive operation
  static const NFA& getPrototypeNFA() {
    static NFA prototypeNFA = RegexParser().parse(getStaticFullRegex());
    return prototypeNFA;
  }

  NFA tokenNFA;

 public:
  // For testing purposes
  NFA& getTokenNFA() { return tokenNFA; }

 public:
  explicit Lexer(std::istream& in) : input(&in) {
    // Ensure the static NFA is built (first call initializes it)
    // Each lexer needs its own NFA for state tracking during tokenization
    // This re-parses but uses the cached regex string
    tokenNFA = RegexParser().parse(getStaticFullRegex());
  }

  // Reset lexer to parse a new input stream without rebuilding NFA
  void resetInput(std::istream& in) {
    input = &in;
    currentChar = ' ';
    currentPos = Position{1, 1, 0};
    buffer.clear();
    tokenNFA.fullReset();
  }

  // Deleted copy operations - NFA contains unique_ptr and can't be copied
  Lexer(const Lexer&) = delete;
  Lexer& operator=(const Lexer&) = delete;
  Lexer(Lexer&&) noexcept = default;
  Lexer& operator=(Lexer&&) noexcept = default;
  ~Lexer() = default;

  std::optional<RegexCapture> getLongestRegexCapture(
      const std::map<int, std::vector<RegexCapture>>& RegexCaptures) {
    if (RegexCaptures.empty()) return std::nullopt;

    std::vector<RegexCapture> longestRegexCaptures;
    for (const auto& [groupIdx, caps] : RegexCaptures) {
      for (const auto& cap : caps) {
        if (cap.groupName.empty()) continue;

        if (longestRegexCaptures.empty()) {
          longestRegexCaptures.push_back(cap);
        } else if (cap.text.size() > longestRegexCaptures[0].text.size()) {
          longestRegexCaptures.clear();
          longestRegexCaptures.push_back(cap);
        } else if (cap.text.size() == longestRegexCaptures[0].text.size()) {
          longestRegexCaptures.push_back(cap);
        }
      }
    }

    // If multiple RegexCaptures have the same length, choose the one with the
    // lowest group index
    RegexCapture longestRegexCapture = longestRegexCaptures[0];
    for (const auto& cap : longestRegexCaptures) {
      if (std::stoi(cap.groupName) < std::stoi(longestRegexCapture.groupName)) {
        longestRegexCapture = cap;
      }
    }
    return longestRegexCapture;
  }

  Token getNextToken() {
    tokenNFA.resetToPosition(currentPos);
    StepResult stepResult;
    while (tokenNFA.canReachAcceptingWithNonEmptyInput()) {
      advance();
      if (currentChar == EOF || input->eof()) break;
      stepResult = tokenNFA.step(currentChar);
    }

    if (stepResult.captures.size() == 0) {
      if (currentChar == EOF) {
        return Token::eof(currentPos);
      }
      throw std::runtime_error("Unrecognized token '" +
                               std::string(1, currentChar) + "' at line " +
                               std::to_string(currentPos.line) + ", column " +
                               std::to_string(currentPos.column));
    }

    auto RegexCapture = getLongestRegexCapture(stepResult.captures);
    if (RegexCapture && !RegexCapture->groupName.empty()) {
      std::string matchedStr = RegexCapture->text;
      Position startPos = RegexCapture->start;
      Position endPos = RegexCapture->end;
      TokenKind kind =
          static_cast<TokenKind>(std::stoi(RegexCapture->groupName));
      setPosition(endPos);

      switch (kind) {
        case TokenKind::COMMENT:
          // Skip comments by recursively getting the next token
          return getNextToken();
        case TokenKind::INTRINSIC_IDENTIFIER:
          return Token::intrinsicIdentifier(matchedStr, startPos, endPos);
        case TokenKind::IDENTIFIER:
          return Token::identifier(matchedStr, startPos, endPos);
        case TokenKind::INTEGER: {
          int64_t val = std::strtoll(matchedStr.c_str(), nullptr, 10);
          return Token::integer(val, startPos, endPos, matchedStr);
        }
        case TokenKind::FLOAT: {
          double val = std::strtod(matchedStr.c_str(), nullptr);
          return Token::floatNum(val, startPos, endPos, matchedStr);
        }
        case TokenKind::STRING: {
          // Remove the surrounding quotes from the matched string
          std::string content = matchedStr.substr(1, matchedStr.size() - 2);
          return Token::stringLiteral(content, startPos, endPos);
        }
        default:
          // All other tokens use the lookup table
          return Token::make(kind, startPos, endPos);
      }
    }
    throw std::runtime_error("Unrecognized token at line " +
                             std::to_string(currentPos.line) + ", column " +
                             std::to_string(currentPos.column));
  }
};