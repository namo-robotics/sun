/** Language server semantic tokens implementation. */

#include <algorithm>
#include <cctype>
#include <sstream>

#include "parsing/lexer.h"
#include "server.h"

using sun::parsing::TokenKind;

/** Implements the Sun language server. */
namespace sun::lsp {

// LSP semantic token type indices (must match legend in initialize response)

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
// Constructors and destructors use the extension's function color mapping.
constexpr int Lifecycle = 22;

/**
 * Map TokenKind to LSP semantic token type index (-1 = skip)
 */
static int tokenKindToLSPType(TokenKind kind) {
  switch (kind) {
    // The grammar distinguishes control flow, declarations, and primitive
    // types. A generic semantic keyword or type would override those scopes.
    case TokenKind::INTEGER:
    case TokenKind::FLOAT:
    case TokenKind::TYPED_INTEGER:
    case TokenKind::TYPED_FLOAT:
      return Number;

    case TokenKind::STRING:
    case TokenKind::CHAR_LITERAL:
    case TokenKind::BYTE_LITERAL:
      return String;

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
    case TokenKind::TILDE:
    case TokenKind::PLUS_ASSIGN:
    case TokenKind::MINUS_ASSIGN:
    case TokenKind::STAR_ASSIGN:
    case TokenKind::SLASH_ASSIGN:
    case TokenKind::PERCENT_ASSIGN:
    case TokenKind::AMP_ASSIGN:
    case TokenKind::PIPE_ASSIGN:
    case TokenKind::CARET_ASSIGN:
    case TokenKind::LEFT_SHIFT_ASSIGN:
    case TokenKind::RIGHT_SHIFT_ASSIGN:
      return Operator;

    case TokenKind::INTRINSIC_IDENTIFIER:
      return Function;

    case TokenKind::COMMENT:
    case TokenKind::BLOCK_COMMENT:
      return Comment;

    default:
      return -1;
  }
}

/**
 * Compute semantic tokens for a document
 * Returns encoded token data as per LSP spec: [deltaLine, deltaStartChar,
 * length, tokenType, tokenModifiers]...
 */
std::vector<int> computeSemanticTokens(const std::string& source) {
  std::vector<int> data;
  std::istringstream stream(source);
  sun::parsing::Lexer lexer(stream);
  lexer.setEmitComments(true);

  // Track context for classifying identifiers as type vs value
  int angleBracketDepth = 0;
  TokenKind prevKind = TokenKind::TOK_EOF;
  bool afterClass = false;
  bool afterEnum = false;
  bool afterModule = false;
  bool afterInterface = false;
  // Implements and extends accept a list that ends at the declaration body.
  bool inInterfaceList = false;
  bool afterFunction = false;
  bool afterColon = false;
  bool afterArrow = false;
  bool afterDot = false;  // For method calls: obj.method()

  /** A source token and its editor semantic-highlighting classification. */
  struct SemanticToken {
    int line;       // 0-indexed
    int startChar;  // 0-indexed
    int length;
    int tokenType;
    int modifiers = 0;
  };
  std::vector<SemanticToken> tokens;

  /**
   * Track last identifier to reclassify on ( or < lookahead
   */
  struct {
    bool valid = false;
    int line;
    int col;
    int length;
    std::string text;
  } lastIdent;

  while (true) {
    sun::parsing::Token tok = lexer.getNextToken();
    if (tok.kind == TokenKind::TOK_EOF) {
      break;
    }

    if (tok.kind == TokenKind::COMMENT ||
        tok.kind == TokenKind::BLOCK_COMMENT) {
      // Emit one token per line (clients may not support multiline tokens).
      // Comments are transparent to the classification context below, so
      // skip the rest of the loop without updating prevKind/lastIdent.
      int commentLine = tok.start.line - 1;
      int commentCol = tok.start.column - 1;
      size_t pos = 0;
      while (pos <= tok.text.size()) {
        size_t nl = tok.text.find('\n', pos);
        size_t segEnd = (nl == std::string::npos) ? tok.text.size() : nl;
        while (segEnd > pos && tok.text[segEnd - 1] == '\r') {
          --segEnd;
        }
        int segLen = static_cast<int>(segEnd - pos);
        if (segLen > 0) {
          tokens.push_back({commentLine, commentCol, segLen, Comment, 0});
        }
        if (nl == std::string::npos) {
          break;
        }
        pos = nl + 1;
        ++commentLine;
        commentCol = 0;
      }
      continue;
    }

    int line = tok.start.line - 1;   // Convert to 0-indexed
    int col = tok.start.column - 1;  // Convert to 0-indexed
    int length = static_cast<int>(tok.text.size());
    if (length == 0) {
      length = tok.end.column - tok.start.column;
      if (length <= 0) {
        length = 1;
      }
    }

    // Try the simple mapping first
    int tokenType = tokenKindToLSPType(tok.kind);

    // Check if previous identifier should be reclassified as function/method
    if (lastIdent.valid) {
      if (tok.kind == TokenKind::PAREN_OPEN || tok.kind == TokenKind::LESS) {
        // Previous identifier is a function/method call
        // Find and update it in tokens. Constructors and destructors (init /
        // deinit) get their own token type so they stand apart from both
        // keywords and ordinary methods.
        bool isLifecycle =
            lastIdent.text == "init" || lastIdent.text == "deinit";
        for (auto& t : tokens) {
          if (t.line == lastIdent.line && t.startChar == lastIdent.col) {
            t.tokenType = isLifecycle ? Lifecycle : Function;
            break;
          }
        }
        // A '<' opening a generic argument list is counted by the context
        // tracking below (prevKind is the identifier); counting it here too
        // would leave the depth stuck above zero after the closing '>'.
      }
      lastIdent.valid = false;
    }

    // Handle context tracking for keywords that affect following tokens
    if (tok.kind == TokenKind::CLASS || tok.kind == TokenKind::PACKED_CLASS) {
      afterClass = true;
    } else if (tok.kind == TokenKind::ENUM) {
      afterEnum = true;
    } else if (tok.kind == TokenKind::MODULE) {
      afterModule = true;
    } else if (tok.kind == TokenKind::INTERFACE) {
      afterInterface = true;
    } else if (tok.kind == TokenKind::IMPLEMENTS ||
               tok.kind == TokenKind::EXTENDS) {
      inInterfaceList = true;
    } else if (tok.kind == TokenKind::FUNCTION ||
               tok.kind == TokenKind::TEST_FUNCTION) {
      afterFunction = true;
    } else if (tok.kind == TokenKind::COLON) {
      afterColon = true;
    } else if (tok.kind == TokenKind::ARROW) {
      afterArrow = true;
    } else if (tok.kind == TokenKind::DOT) {
      afterDot = true;
    } else if (tok.kind == TokenKind::LESS) {
      // Could be generic or comparison - check context
      if (prevKind == TokenKind::IDENTIFIER || afterClass || afterInterface ||
          afterFunction) {
        angleBracketDepth++;
      }
    } else if (tok.kind == TokenKind::GREATER && angleBracketDepth > 0) {
      angleBracketDepth--;
    } else if (tok.kind == TokenKind::RIGHT_SHIFT && angleBracketDepth > 0) {
      // '>>' closing two nested generic lists lexes as one token
      angleBracketDepth = std::max(0, angleBracketDepth - 2);
    } else if (tok.kind == TokenKind::COMMA) {
      afterColon = false;
      afterArrow = false;
    } else if (tok.kind == TokenKind::PAREN_OPEN) {
      // Reset declaration context but keep angle brackets for generic calls
      afterEnum = afterModule = false;
      afterClass = afterInterface = inInterfaceList = afterFunction = false;
      afterColon = afterArrow = afterDot = false;
    } else if (tok.kind == TokenKind::PAREN_CLOSE ||
               tok.kind == TokenKind::BRACE_OPEN ||
               tok.kind == TokenKind::SEMI_COLON) {
      afterEnum = afterModule = false;
      afterClass = afterInterface = inInterfaceList = afterFunction = false;
      afterColon = afterArrow = afterDot = false;
      if (tok.kind == TokenKind::BRACE_OPEN ||
          tok.kind == TokenKind::SEMI_COLON) {
        angleBracketDepth = 0;
      }
    } else if (tok.kind == TokenKind::BRACE_CLOSE) {
      angleBracketDepth = 0;
    }

    // Identifiers need context-based classification
    else if (tok.kind == TokenKind::IDENTIFIER) {
      bool isTypeContext = afterColon || afterArrow || inInterfaceList ||
                           afterClass || afterInterface ||
                           angleBracketDepth > 0;

      // `method` is a keyword only where a class or interface member starts,
      // so a field named `method` or a `req.method` access stays an
      // identifier. The name after it is the method's.
      if (tok.text == "method" &&
          (prevKind == TokenKind::BRACE_OPEN ||
           prevKind == TokenKind::BRACE_CLOSE ||
           prevKind == TokenKind::SEMI_COLON || prevKind == TokenKind::PUBLIC ||
           prevKind == TokenKind::CONST)) {
        tokenType = -1;
        afterFunction = true;
      } else if (afterEnum) {
        tokenType = Enum;
        afterEnum = false;
      } else if (afterModule) {
        tokenType = Module;
        afterModule = false;
      } else if (afterClass) {
        tokenType = Class;
        afterClass = false;
      } else if (afterInterface) {
        tokenType = Interface;
        afterInterface = false;
      } else if (afterFunction) {
        tokenType = Function;
        afterFunction = false;
        // Track this - might be followed by < for generics
        lastIdent = {true, line, col, length, tok.text};
      } else if (isTypeContext) {
        if (angleBracketDepth > 0 && tok.text.size() <= 2 &&
            std::isupper(tok.text[0])) {
          tokenType = TypeParameter;
        } else {
          tokenType = Type;
        }
      } else if (afterDot) {
        // Member access - could be property or method
        // Default to property, will be reclassified to method if followed by (
        tokenType = Property;
        lastIdent = {true, line, col, length, tok.text};
        afterDot = false;
      } else {
        tokenType = Variable;
        // Track for potential function call: foo() or foo<T>()
        lastIdent = {true, line, col, length, tok.text};
      }
      afterColon = afterArrow = false;
    }

    // Special case for literal length (includes the quotes). Without this a
    // literal has length 0 and is dropped below.
    if (tok.kind == TokenKind::STRING || tok.kind == TokenKind::CHAR_LITERAL ||
        tok.kind == TokenKind::BYTE_LITERAL) {
      length = tok.end.column - tok.start.column;
    }

    if (tokenType >= 0 && length > 0) {
      tokens.push_back({line, col, length, tokenType, 0});
    }

    if ((tok.kind >= TokenKind::TYPE_I8 && tok.kind <= TokenKind::TYPE_CHAR) ||
        tok.kind == TokenKind::ARRAY) {
      afterColon = afterArrow = false;
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

}  // namespace sun::lsp
