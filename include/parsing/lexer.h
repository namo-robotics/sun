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

#include "support/error.h"
#include "parsing/nfa.h"

enum class TokenKind {
  TOK_EOF,
  COMMENT,        // // comment (skipped unless emitComments is set)
  BLOCK_COMMENT,  // /* comment */ (non-nested; skipped unless emitComments)
  DECLARE,        // declare keyword for explicit generic instantiation
  DEF,
  EXTERN,
  VAR,
  MANIFEST,       // manifest keyword for module metadata
  MODULE,         // module keyword (preferred over namespace)
  USING,          // using keyword for namespace imports
  CLASS,          // class keyword
  PARTIAL,        // partial keyword for class extensions
  PUBLIC,         // public keyword: visibility modifier (private is the default)
  PACKED_CLASS,   // packed_class keyword: class with no inter-field padding
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
  SPAWN,     // spawn keyword for OS thread creation
  UNSAFE,    // unsafe keyword for unsafe blocks
  // Type keywords (must come before IDENTIFIER for priority)
  STATIC_PTR,            // static_ptr (pointer to immortal static data)
  PTR,                   // ptr (unique/owning pointer with RAII)
  RAW_PTR,               // raw_ptr (non-owning pointer for C interop)
  REF,                   // ref (reference type)
  CONST,                 // const (constant variable / const ref / const method)
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
  TEMPLATE_STRING,       // template string literal `...` (may contain ${expr})
  BRACE_OPEN,            // {
  BRACE_CLOSE,           // }
  BRACKET_OPEN,          // [
  BRACKET_CLOSE,         // ]
  PLUS,                  // +
  MINUS,                 // -
  STAR,                  // *
  SLASH,                 // /
  PERCENT,               // %
  AMPERSAND,             // & (bitwise AND)
  PIPE,                  // | (bitwise OR)
  CARET,                 // ^ (bitwise XOR)
  LEFT_SHIFT,            // <<
  RIGHT_SHIFT,           // >>
  TILDE,                 // ~ (bitwise NOT)
  // Compound assignment operators (desugared in the parser; never serialized)
  PLUS_ASSIGN,           // +=
  MINUS_ASSIGN,          // -=
  STAR_ASSIGN,           // *=
  SLASH_ASSIGN,          // /=
  PERCENT_ASSIGN,        // %=
  AMP_ASSIGN,            // &=
  PIPE_ASSIGN,           // |=
  CARET_ASSIGN,          // ^=
  LEFT_SHIFT_ASSIGN,     // <<=
  RIGHT_SHIFT_ASSIGN,    // >>=
  AND,                   // and (logical AND)
  OR,                    // or (logical OR)
  NOT,                   // not (logical NOT)
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
  QUESTION,              // ?
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
    {TokenKind::COMMENT, "//[^\r\n]*"},  // Line comments
    // Non-nested block comments; cannot match past the first */
    {TokenKind::BLOCK_COMMENT, "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/"},
    {TokenKind::DECLARE, "declare"},
    {TokenKind::DEF, "def"},
    {TokenKind::EXTERN, "extern"},
    {TokenKind::VAR, "var"},
    {TokenKind::MANIFEST, "manifest"},
    {TokenKind::MODULE, "module"},
    {TokenKind::USING, "using"},
    {TokenKind::CLASS, "class"},
    {TokenKind::PARTIAL, "partial"},
    {TokenKind::PUBLIC, "public"},
    // Must precede CLASS's regex conceptually; longest-match makes
    // "packed_class" win over both "class" and IDENTIFIER
    {TokenKind::PACKED_CLASS, "packed_class"},
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
    {TokenKind::SPAWN, "spawn"},
    {TokenKind::UNSAFE, "unsafe"},
    {TokenKind::FAT_ARROW, "=>"},
    {TokenKind::ARROW, "->"},
    {TokenKind::UNDERSCORE, "_"},
    {TokenKind::STATIC_PTR, "static_ptr"},  // Must come before ptr
    {TokenKind::RAW_PTR, "raw_ptr"},        // Must come before ptr
    {TokenKind::PTR, "ptr"},
    {TokenKind::REF, "ref"},
    {TokenKind::CONST, "const"},
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
    {TokenKind::STRING, "\"([^\"\\\\]|\\\\.)*\""},
    {TokenKind::TEMPLATE_STRING, "`([^`\\\\]|\\\\.)*`"},
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
    {TokenKind::PERCENT, "%"},
    {TokenKind::AND, "and"},
    {TokenKind::OR, "or"},
    {TokenKind::NOT, "not"},
    {TokenKind::LEFT_SHIFT, "<<"},
    {TokenKind::RIGHT_SHIFT, ">>"},
    {TokenKind::TILDE, "~"},
    {TokenKind::PLUS_ASSIGN, "\\+="},
    {TokenKind::MINUS_ASSIGN, "-="},
    {TokenKind::STAR_ASSIGN, "\\*="},
    {TokenKind::SLASH_ASSIGN, "/="},
    {TokenKind::PERCENT_ASSIGN, "%="},
    {TokenKind::AMP_ASSIGN, "&="},
    {TokenKind::PIPE_ASSIGN, "\\|="},
    {TokenKind::CARET_ASSIGN, "\\^="},
    {TokenKind::LEFT_SHIFT_ASSIGN, "<<="},
    {TokenKind::RIGHT_SHIFT_ASSIGN, ">>="},
    {TokenKind::AMPERSAND, "&"},
    {TokenKind::PIPE, "\\|"},
    {TokenKind::CARET, "\\^"},
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
    {TokenKind::QUESTION, "\\?"},
    {TokenKind::ELLIPSIS, "\\.\\.\\."},
    {TokenKind::DOT, "\\."}};

// Map compound-assignment token kinds (+=, -=, ...) to the underlying
// binary operator; nullopt for anything else. Shared by the parser,
// semantic analysis, and codegen.
inline std::optional<TokenKind> compoundToBinaryOp(TokenKind kind) {
  switch (kind) {
    case TokenKind::PLUS_ASSIGN:
      return TokenKind::PLUS;
    case TokenKind::MINUS_ASSIGN:
      return TokenKind::MINUS;
    case TokenKind::STAR_ASSIGN:
      return TokenKind::STAR;
    case TokenKind::SLASH_ASSIGN:
      return TokenKind::SLASH;
    case TokenKind::PERCENT_ASSIGN:
      return TokenKind::PERCENT;
    case TokenKind::AMP_ASSIGN:
      return TokenKind::AMPERSAND;
    case TokenKind::PIPE_ASSIGN:
      return TokenKind::PIPE;
    case TokenKind::CARET_ASSIGN:
      return TokenKind::CARET;
    case TokenKind::LEFT_SHIFT_ASSIGN:
      return TokenKind::LEFT_SHIFT;
    case TokenKind::RIGHT_SHIFT_ASSIGN:
      return TokenKind::RIGHT_SHIFT;
    default:
      return std::nullopt;
  }
}

// The spelling of a keyword token (a token whose regex is a bare word, so it
// would otherwise lex as an identifier); nullopt for any other kind. Derived
// from the regex table so a new keyword is covered without a second list.
inline std::optional<std::string_view> getKeywordSpelling(TokenKind kind) {
  static const std::array<std::optional<std::string_view>,
                          static_cast<size_t>(TokenKind::COUNT)>
      table = [] {
        std::array<std::optional<std::string_view>,
                   static_cast<size_t>(TokenKind::COUNT)>
            a{};
        for (const auto& [k, re] : tokenRegexes) {
          if (k == TokenKind::IDENTIFIER || re.empty() ||
              !std::isalpha(static_cast<unsigned char>(re[0])))
            continue;
          bool word = true;
          for (char c : re) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
              word = false;
              break;
            }
          }
          if (word) a[static_cast<size_t>(k)] = std::string_view(re);
        }
        return a;
      }();
  return table[static_cast<size_t>(kind)];
}

inline bool isKeyword(TokenKind kind) {
  return getKeywordSpelling(kind).has_value();
}

// Lookup table for simple token metadata (text and precedence)
struct TokenInfo {
  std::string_view text;
  int precedence = -1;
};

inline const std::map<TokenKind, TokenInfo>& getTokenInfo() {
  static const std::map<TokenKind, TokenInfo> tokenInfo = {
      {TokenKind::TOK_EOF, {""}},
      {TokenKind::DECLARE, {"declare"}},
      {TokenKind::DEF, {"def"}},
      {TokenKind::EXTERN, {"extern"}},
      {TokenKind::VAR, {"var"}},
      {TokenKind::MANIFEST, {"manifest"}},
      {TokenKind::MODULE, {"module"}},
      {TokenKind::USING, {"using"}},
      {TokenKind::CLASS, {"class"}},
      {TokenKind::PARTIAL, {"partial"}},
      {TokenKind::PUBLIC, {"public"}},
      {TokenKind::PACKED_CLASS, {"packed_class"}},
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
      {TokenKind::SPAWN, {"spawn"}},
      {TokenKind::UNSAFE, {"unsafe"}},
      {TokenKind::STATIC_PTR, {"static_ptr"}},
      {TokenKind::PTR, {"ptr"}},
      {TokenKind::RAW_PTR, {"raw_ptr"}},
      {TokenKind::REF, {"ref"}},
      {TokenKind::CONST, {"const"}},
      {TokenKind::ARRAY, {"array"}},
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
      {TokenKind::QUESTION, {"?"}},
      {TokenKind::ELLIPSIS, {"..."}},
      {TokenKind::DOT, {"."}},
      {TokenKind::EQUAL, {"="}},
      {TokenKind::UNKNOWN, {""}},
      // Operators with precedence
      {TokenKind::PLUS, {"+", 20}},
      {TokenKind::MINUS, {"-", 20}},
      {TokenKind::STAR, {"*", 40}},
      {TokenKind::SLASH, {"/", 40}},
      {TokenKind::PERCENT, {"%", 40}},
      {TokenKind::LEFT_SHIFT, {"<<", 15}},
      {TokenKind::RIGHT_SHIFT, {">>", 15}},
      {TokenKind::AMPERSAND, {"&", 8}},
      {TokenKind::CARET, {"^", 7}},
      {TokenKind::PIPE, {"|", 6}},
      {TokenKind::AND, {"and", 5}},
      {TokenKind::OR, {"or", 4}},
      {TokenKind::NOT, {"not", -1}},
      {TokenKind::TILDE, {"~", -1}},
      {TokenKind::PLUS_ASSIGN, {"+=", -1}},
      {TokenKind::MINUS_ASSIGN, {"-=", -1}},
      {TokenKind::STAR_ASSIGN, {"*=", -1}},
      {TokenKind::SLASH_ASSIGN, {"/=", -1}},
      {TokenKind::PERCENT_ASSIGN, {"%=", -1}},
      {TokenKind::AMP_ASSIGN, {"&=", -1}},
      {TokenKind::PIPE_ASSIGN, {"|=", -1}},
      {TokenKind::CARET_ASSIGN, {"^=", -1}},
      {TokenKind::LEFT_SHIFT_ASSIGN, {"<<=", -1}},
      {TokenKind::RIGHT_SHIFT_ASSIGN, {">>=", -1}},
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

  // Generic factory for simple tokens (uses lookup table).
  // Indexes a flat array rather than searching getTokenInfo()'s std::map:
  // this runs once per operator/keyword/punctuation token, and a red-black
  // tree walk per token was measurable in the lexer's profile.
  static Token make(TokenKind k, const Position& s, const Position& e) {
    static const std::array<const TokenInfo*, static_cast<size_t>(
                                                  TokenKind::COUNT)>
        byKind = [] {
          std::array<const TokenInfo*, static_cast<size_t>(TokenKind::COUNT)>
              a{};
          for (const auto& [kind, info] : getTokenInfo()) {
            a[static_cast<size_t>(kind)] = &info;
          }
          return a;
        }();

    const TokenInfo* info = byKind[static_cast<size_t>(k)];
    if (info) {
      return {k, std::monostate{}, s, e, std::string(info->text),
              info->precedence};
    }
    return {k, std::monostate{}, s, e, "", -1};
  }

  // Factories for value-carrying tokens
  static Token eof(const Position& pos) { return make(TokenKind::TOK_EOF, pos, pos); }

  static Token identifier(std::string id, const Position& s, const Position& e) {
    return {TokenKind::IDENTIFIER, id, s, e, std::move(id)};
  }

  static Token intrinsicIdentifier(std::string id, const Position& s, const Position& e) {
    return {TokenKind::INTRINSIC_IDENTIFIER, id, s, e, std::move(id)};
  }

  static Token integer(int64_t num, const Position& s, const Position& e, std::string txt) {
    return {TokenKind::INTEGER, num, s, e, std::move(txt)};
  }

  static Token floatNum(double num, const Position& s, const Position& e, std::string txt) {
    return {TokenKind::FLOAT, num, s, e, std::move(txt)};
  }

  static Token stringLiteral(std::string str, const Position& s, const Position& e) {
    return {TokenKind::STRING, std::move(str), s, e, ""};
  }

  // Comment token factory (COMMENT or BLOCK_COMMENT); text is the raw
  // comment including delimiters
  static Token comment(TokenKind k, std::string txt, const Position& s, const Position& e) {
    return {k, txt, s, e, std::move(txt)};
  }

  // Template string token factory
  static Token templateString(std::string str, const Position& s, const Position& e) {
    return {TokenKind::TEMPLATE_STRING, std::move(str), s, e, ""};
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

  // Get template string content
  std::optional<std::string> getTemplateString() const {
    if (kind == TokenKind::TEMPLATE_STRING) {
      return std::get<std::string>(value);
    }
    return std::nullopt;
  }
};

class Lexer {
 public:
  // End of input. Distinct from any byte value: currentChar is an int so that
  // byte 0xFF does not alias EOF the way a signed char would.
  static constexpr int kEof = -1;

 private:
  // The whole input is read up front, so scanning never touches the stream.
  // Reading byte-at-a-time through istream::get() cost ~70% of lexing time
  // (virtual streambuf dispatch + sentry per byte, plus growing the buffer one
  // char at a time); a slurped buffer makes advance() a load and two adds.
  int currentChar = ' ';
  DFA* dfa_ = &getTokenDFA();
  Position currentPos{1, 1, 0};
  // When set, comments are returned as tokens instead of skipped
  // (deliberately preserved across resetInput)
  bool emitComments_ = false;

  std::string buffer;

  static bool isTokenWhitespace(int c) {
    return c == ' ' || c == '\n' || c == '\t' || c == '\r';
  }

  // Process escape sequences in regular string literals.
  // Mirrors InterpolatedStringParser::processEscapes (template strings),
  // with \" instead of the template-specific \` and \$.
  static std::string processStringEscapes(std::string_view raw) {
    std::string result;
    result.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
      if (raw[i] == '\\' && i + 1 < raw.size()) {
        char next = raw[i + 1];
        switch (next) {
          case '"':
            result += '"';
            break;
          case 'n':
            result += '\n';
            break;
          case 't':
            result += '\t';
            break;
          case 'r':
            result += '\r';
            break;
          case '\\':
            result += '\\';
            break;
          case '0':
            result += '\0';
            break;
          default:
            // Unknown escape - keep as-is
            result += raw[i];
            result += next;
            break;
        }
        i++;  // Skip the escaped character
      } else {
        result += raw[i];
      }
    }
    return result;
  }

  // Read the entire stream into buffer. Called once per input.
  // Bulk reads, not istreambuf_iterator: the iterator form goes through the
  // streambuf one character at a time and cost ~20% of total lexing
  // instructions, which is the very per-byte overhead the slurp exists to
  // avoid. istream::read() hands off to sgetn() and memcpys whole chunks.
  void slurp(std::istream& in) {
    buffer.clear();
    char chunk[64 * 1024];
    while (in.read(chunk, sizeof chunk) || in.gcount() > 0) {
      buffer.append(chunk, static_cast<size_t>(in.gcount()));
    }
  }

  // Consume one byte and advance line/column. Returns kEof at end of input,
  // otherwise the byte value in 0..255. This is the single owner of the
  // lexer's position: nothing else moves currentPos forward.
  int advance() {
    if (currentPos.offset >= static_cast<int>(buffer.size())) {
      currentChar = kEof;
      return kEof;
    }
    currentChar = static_cast<unsigned char>(buffer[currentPos.offset]);

    ++currentPos.offset;
    if (currentChar == '\n') {
      ++currentPos.line;
      currentPos.column = 1;
    } else {
      ++currentPos.column;
    }

    return currentChar;
  }

  int peekByte() const {
    if (currentPos.offset >= static_cast<int>(buffer.size())) return kEof;
    return static_cast<unsigned char>(buffer[currentPos.offset]);
  }

  // Commit a scan position without a whole-Position copy-assign. Position
  // carries an optional<std::string> filePath and three optional<int>s that
  // the lexer never sets, and assigning them cost one optional<string>
  // copy-assignment per token. Only the three coordinates actually change.
  void commitPosition(int line, int col, int off) {
    currentPos.line = line;
    currentPos.column = col;
    currentPos.offset = off;
    currentChar = static_cast<unsigned char>(buffer[off]);
  }

 public:
  void setPosition(const Position& pos) {
    currentPos = pos;
    // pos.offset == buffer.size() is routine (rewinding to the end of the last
    // token at EOF); indexing the terminating null there is well-defined.
    currentChar = static_cast<unsigned char>(buffer[pos.offset]);
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

  // Get a specific line from the source buffer (1-indexed)
  std::string getSourceLine(int lineNum) const {
    if (lineNum < 1 || buffer.empty()) return "";

    int currentLine = 1;
    size_t lineStart = 0;

    // Find the start of the requested line
    for (size_t i = 0; i < buffer.size(); ++i) {
      if (currentLine == lineNum) {
        lineStart = i;
        break;
      }
      if (buffer[i] == '\n') {
        ++currentLine;
        if (currentLine == lineNum) {
          lineStart = i + 1;
          break;
        }
      }
    }

    if (currentLine < lineNum) return "";  // Line not found

    // Find the end of the line
    size_t lineEnd = lineStart;
    while (lineEnd < buffer.size() && buffer[lineEnd] != '\n') {
      ++lineEnd;
    }

    return buffer.substr(lineStart, lineEnd - lineStart);
  }

 public:
  // Build the full regex string once (expensive string operations).
  // Public so tests can determinize the same pattern the lexer uses.
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

  // One process-wide token DFA, shared by every Lexer. Per-scan state is just
  // an int, so nothing here is per-instance; scanning only grows the lazily
  // built transition cache. That mutation is safe because the compiler and the
  // LSP are single-threaded. If that ever changes, make this thread_local or
  // guard DFA::step()'s miss path with a mutex.
  static DFA& getTokenDFA() {
    static DFA tokenDFA = RegexParser().parse(getStaticFullRegex());
    return tokenDFA;
  }

  explicit Lexer(std::istream& in) { slurp(in); }

  void setEmitComments(bool emit) { emitComments_ = emit; }
  bool emitComments() const { return emitComments_; }

  // Point the lexer at a new input. There is no per-lexer scan state beyond
  // the buffer and position; the token DFA is shared and stateless.
  void resetInput(std::istream& in) {
    currentChar = ' ';
    currentPos = Position{1, 1, 0};
    slurp(in);
  }

  // Copying a Lexer would duplicate a position into a shared stream; move only
  Lexer(const Lexer&) = delete;
  Lexer& operator=(const Lexer&) = delete;
  Lexer(Lexer&&) noexcept = default;
  Lexer& operator=(Lexer&&) noexcept = default;
  ~Lexer() = default;

  Token getNextToken() {
    // Cached at construction: getTokenDFA() is a function-local static, so
    // calling it per token pays the thread-safe-init guard every time.
    DFA& dfa = *dfa_;

    // The scan runs on locals rather than through advance(), so the position
    // triple stays in registers instead of being written to members on every
    // byte; currentPos is committed once, at the end, via setPosition().
    // buffer never grows during a scan (the input is slurped up front), so
    // data stays valid throughout.
    const char* const data = buffer.data();
    const int size = static_cast<int>(buffer.size());
    int off = currentPos.offset;
    int line = currentPos.line;
    int col = currentPos.column;

    // Skip leading whitespace directly. Equivalent to the master regex's
    // "[ \n\t\r]*" prefix: no token pattern starts with whitespace, and the
    // prefix sits outside every named group, so the match starts here.
    while (off < size) {
      const char c = data[off];
      if (c == ' ' || c == '\t' || c == '\r') {
        ++off;
        ++col;
      } else if (c == '\n') {
        ++off;
        ++line;
        col = 1;
      } else {
        break;
      }
    }

    const int startOffset = off;
    const int startLine = line;
    const int startCol = col;

    if (off >= size) {
      commitPosition(line, col, off);
      return Token::eof(currentPos);
    }

    // Longest match wins; ties go to the lowest TokenKind index, which the DFA
    // precomputes as each state's acceptKind. The position triple is
    // snapshotted at every accept -- so rewinding to the last accept restores
    // the right line/column even for a match spanning newlines (block
    // comments, multi-line strings) with no recomputation.
    int32_t st = dfa.startState();
    int32_t bestKind = DFA::kNoAccept;
    int bestOffset = startOffset;
    int bestLine = startLine;
    int bestCol = startCol;

    while (off < size) {
      const unsigned char c = static_cast<unsigned char>(data[off]);
      st = dfa.step(st, c);
      ++off;
      if (c == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
      if (DFA::isDead(st)) break;
      const int32_t kindHere = dfa.acceptKind(st);
      if (kindHere >= 0) {
        bestKind = kindHere;
        bestOffset = off;
        bestLine = line;
        bestCol = col;
      }
    }

    if (bestKind < 0 || bestOffset == startOffset) {
      Position at{startLine, startCol, startOffset};
      commitPosition(startLine, startCol, startOffset);
      std::string sourceLine = getSourceLine(at.line);
      logParsingError(
          at, "Unrecognized token '" + std::string(1, buffer[startOffset]) + "'",
          sourceLine, at.line > 1 ? getSourceLine(at.line - 1) : "");
    }

    Position startPos{startLine, startCol, startOffset};
    Position endPos{bestLine, bestCol, bestOffset};
    TokenKind kind = static_cast<TokenKind>(bestKind);
    commitPosition(bestLine, bestCol, bestOffset);

    // The matched bytes, as a view into the lexer's own buffer. Only the
    // value-carrying kinds below materialize a string from it -- operators,
    // keywords and punctuation take their text from the static table, and a
    // skipped comment needs no text at all.
    const std::string_view matched(buffer.data() + startOffset,
                                   static_cast<size_t>(bestOffset - startOffset));

    switch (kind) {
      case TokenKind::COMMENT:
      case TokenKind::BLOCK_COMMENT:
        if (emitComments_)
          return Token::comment(kind, std::string(matched), startPos, endPos);
        // Skip comments by recursively getting the next token
        return getNextToken();
      case TokenKind::INTRINSIC_IDENTIFIER:
        return Token::intrinsicIdentifier(std::string(matched), startPos,
                                          endPos);
      case TokenKind::IDENTIFIER:
        return Token::identifier(std::string(matched), startPos, endPos);
      case TokenKind::INTEGER: {
        std::string text(matched);
        int64_t val = std::strtoll(text.c_str(), nullptr, 10);
        return Token::integer(val, startPos, endPos, std::move(text));
      }
      case TokenKind::FLOAT: {
        std::string text(matched);
        double val = std::strtod(text.c_str(), nullptr);
        return Token::floatNum(val, startPos, endPos, std::move(text));
      }
      case TokenKind::STRING: {
        // Drop the surrounding quotes and process escape sequences
        return Token::stringLiteral(
            processStringEscapes(matched.substr(1, matched.size() - 2)),
            startPos, endPos);
      }
      case TokenKind::TEMPLATE_STRING: {
        // Remove the surrounding backticks from the matched string
        return Token::templateString(
            std::string(matched.substr(1, matched.size() - 2)), startPos,
            endPos);
      }
      default:
        // All other tokens use the lookup table
        return Token::make(kind, startPos, endPos);
    }
  }
};