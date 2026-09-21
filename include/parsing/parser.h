#pragma once

#include <iostream>
#include <istream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "ast.h"
#include "ast/manifest_ast.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_import.h"
#include "parsing/lexer.h"
#include "support/error.h"

/** Turns Sun source text into tokens and syntax trees. */
namespace sun::parsing {
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;

using std::unique_ptr;

/**
 * The actual C function that prints a character
 */
static double putchard(double X) {
  char c = static_cast<char>(X);
  putchar(c);
  fflush(stdout);  // Optional: ensure immediate output
  return 0.0;      // Kaleidoscope externs return double
}

/** Stores source comments and their positions for the formatter. */
struct Comment {
  sun::support::Position span;  // start + end{Line,Column,Offset}
  std::string text;  // raw text including delimiters ("// x", "/* x */")
  bool ownLine;      // nothing but whitespace/comments precede it on its line
  bool isBlock;      // /* */ vs //
};

/** Parses Sun source into syntax trees and collects source comments. */
class Parser {
 private:
  Lexer lexer;
  Token curTok = Token::eof({0, 0, 0});
  Token prevTok_ = Token::eof({0, 0, 0});  // Last consumed token (span ends)
  std::vector<Token> tokenStack;

  // Track whether string interpolation was used during parsing
  bool usesStringInterpolation_ = false;

  // True while parsing module-level items (program root or a module body);
  // false inside function/control-flow blocks. Gates the `public` modifier.
  bool atItemLevel_ = true;

  // Comment side table, keyed by start offset. Offset keying makes
  // collection idempotent when backtracking re-lexes a region.
  bool collectComments_ = false;
  std::map<int, Comment> comments_;

  /** Reports whether a token contains a source comment. */
  static bool isCommentToken(const Token& tok) {
    return tok.kind == TokenKind::COMMENT ||
           tok.kind == TokenKind::BLOCK_COMMENT;
  }

  /** Retains a comment and its attachment position for source formatting. */
  void recordComment(const Token& tok, int lastEndLine) {
    sun::support::Position span = tok.start;
    if (!currentFilePath.empty()) span.filePath = currentFilePath;
    span.setEnd(tok.end.line, tok.end.column, tok.end.offset);
    comments_.insert_or_assign(
        tok.start.offset,
        Comment{std::move(span), tok.text,
                /*ownLine=*/lastEndLine != tok.start.line,
                /*isBlock=*/tok.kind == TokenKind::BLOCK_COMMENT});
  }

  // Track import paths that should be loaded from precompiled libraries
  // (not parsed from source)
  std::shared_ptr<std::vector<std::string>> precompiledImports =
      std::make_shared<std::vector<std::string>>();

  // Base directory for resolving relative imports
  std::string baseDir;

  // Current file being parsed (for error messages)
  std::string currentFilePath;
  sun::support::SourceFileId sourceFileId_ = sun::support::nextSourceFileId();

  /**
   * Helper: throw parsing error with source context
   */
  [[noreturn]] void parsingError(const std::string& msg) {
    std::string sourceLine = lexer.getSourceLine(curTok.start.line);
    std::string prevLine =
        curTok.start.line > 1 ? lexer.getSourceLine(curTok.start.line - 1) : "";
    sun::support::Position loc{
        curTok.start.line, curTok.start.column, curTok.start.offset,
        currentFilePath.empty() ? std::nullopt
                                : std::optional<std::string>(currentFilePath)};
    loc.setEnd(curTok.end.line, curTok.end.column, curTok.end.offset);
    sun::support::logParsingError(loc, msg, sourceLine, prevLine);
  }

  /**
   * Helper: throw the error for a missing identifier. A keyword standing
   * where a name belongs is reported as such, since "expected identifier"
   * reads as a syntax problem rather than a name collision.
   */
  [[noreturn]] void throwIdentifierError(const std::string& msg) {
    if (auto word = getKeywordSpelling(curTok.kind)) {
      parsingError("'" + std::string(*word) +
                   "' is a reserved word and cannot be used as an identifier");
    }
    parsingError(msg);
  }

  /**
   * Helper: expect current token to be an identifier, or throw error
   */
  void expectIdentifier(const std::string& msg) {
    if (curTok.kind != TokenKind::IDENTIFIER) throwIdentifierError(msg);
  }

  /**
   * Helper: expect current token to be a specific kind, or throw error
   */
  void expectCurrentTokenKind(TokenKind expected, const std::string& msg) {
    if (expected == TokenKind::IDENTIFIER) {
      expectIdentifier(msg);
      return;
    }
    if (curTok.kind != expected) {
      parsingError(msg);
    }
  }

  /**
   * Helper: consume a '>' token, handling '>>' split for nested generics
   * Returns true if consumed, throws error with msg if not
   */
  void consumeGreater(const std::string& msg) {
    if (curTok.kind == TokenKind::GREATER) {
      getNextToken();  // eat '>'
      return;
    }
    if (curTok.kind == TokenKind::RIGHT_SHIFT ||
        curTok.kind == TokenKind::GREATER_EQUAL ||
        curTok.kind == TokenKind::RIGHT_SHIFT_ASSIGN) {
      // Split off the leading '>' and push the remainder back. Spans are
      // split at the character boundary so type annotations sliced from
      // source don't absorb the remainder (e.g. Vec<Vec<i32>>).
      TokenKind remainderKind =
          curTok.kind == TokenKind::RIGHT_SHIFT     ? TokenKind::GREATER
          : curTok.kind == TokenKind::GREATER_EQUAL ? TokenKind::EQUAL
                                                    : TokenKind::GREATER_EQUAL;
      sun::support::Position mid = curTok.start;
      mid.column += 1;
      mid.offset += 1;
      Token remainder = Token::make(remainderKind, mid, curTok.end);
      curTok = Token::make(TokenKind::GREATER, curTok.start, mid);
      getNextToken();  // eat the shrunk '>'
      pushToken(remainder);
      return;
    }
    parsingError(msg);
  }

  /**
   * Helper: when the current token is '<<' (lexed as one shift token),
   * split it into two '<' so a '<'a>' lambda-type marker can open right
   * after a generic argument list: Box<<'a>() => i32>.
   */
  void splitLessIfShift() {
    if (curTok.kind != TokenKind::LEFT_SHIFT) return;
    sun::support::Position mid = curTok.start;
    mid.column += 1;
    mid.offset += 1;
    Token remainder = Token::make(TokenKind::LESS, mid, curTok.end);
    curTok = Token::make(TokenKind::LESS, curTok.start, mid);
    pushToken(remainder);
  }

  /**
   * Helper: check if current token starts with '>' (for lookahead)
   */
  bool isGreater() const {
    return curTok.kind == TokenKind::GREATER ||
           curTok.kind == TokenKind::RIGHT_SHIFT ||
           curTok.kind == TokenKind::GREATER_EQUAL ||
           curTok.kind == TokenKind::RIGHT_SHIFT_ASSIGN;
  }

 public:
  /** Initializes a parser with empty input and default parsing state. */
  Parser() : lexer(std::cin) {}
  /**
   * Updated constructor: takes both input stream and codegen context
   */
  Parser(std::istream& input) : lexer(input) {}

  /**
   * Set the file path for error messages
   */
  void setFilePath(const std::string& path) { currentFilePath = path; }

  /** Parse an embedded expression in its enclosing source unit. */
  void setSourceFileId(sun::support::SourceFileId id) { sourceFileId_ = id; }
  /** Returns the file path stored by this object. */
  const std::string& getFilePath() const { return currentFilePath; }

  /** Consumes tokens for a complete program and builds its syntax-tree
   * representation. */
  unique_ptr<BlockExprAST> parseProgram();
  // Convenience constructors (optional but recommended)

  /** Creates a parser reading the supplied source string. */
  static Parser createStringParser(const std::string& source) {
    auto* ss = new std::istringstream(source);
    Parser parser(*ss);
    parser.getNextToken();  // Prime the lexer
    return parser;
  }

  /** Returns a token to the parser's lookahead buffer. */
  void pushToken(const Token& token) {
    tokenStack.push_back(curTok);
    curTok = token;
  }

  /**
   * Parsing functions
   */
  Token getNextToken() {
    prevTok_ = curTok;
    if (!tokenStack.empty()) {
      curTok = tokenStack.back();
      tokenStack.pop_back();
      return curTok;
    }
    Token tok = lexer.getNextToken();
    // Comments never become curTok/prevTok_: record them (when collecting)
    // and keep fetching. lastEndLine distinguishes own-line comments from
    // trailing ones (prevTok_ starts as the line-0 EOF sentinel).
    int lastEndLine = prevTok_.end.line;
    while (isCommentToken(tok)) {
      recordComment(tok, lastEndLine);
      lastEndLine = tok.end.line;
      tok = lexer.getNextToken();
    }
    curTok = tok;
    return curTok;
  }

  /**
   * Start position of the node about to be parsed (curTok is its first token)
   */
  sun::support::Position captureStart() const {
    sun::support::Position p = curTok.start;
    if (!currentFilePath.empty()) p.filePath = currentFilePath;
    return p;
  }

  /**
   * Move a node's span start back to `start` (e.g. over a leading modifier)
   */
  static void extendSpanStart(ExprAST& node, sun::support::Position start) {
    const sun::support::Position& cur = node.getLocation();
    if (cur.hasEnd())
      start.setEnd(*cur.endLine, *cur.endColumn, cur.endOffset.value_or(0));
    node.setLocation(std::move(start));
  }

  /**
   * Stamp span [start, end-of-last-consumed-token] onto a finished node
   */
  template <typename NodeT>
  unique_ptr<NodeT> finishNode(unique_ptr<NodeT> node,
                               sun::support::Position start) const {
    if (node) {
      start.setEnd(prevTok_.end.line, prevTok_.end.column, prevTok_.end.offset);
      node->setLocation(std::move(start));
      node->inheritSourceFile(sourceFileId_);
    }
    return node;
  }

  /**
   * Span variant for left-recursive constructs: start comes from an
   * already-stamped sub-node's location
   */
  void extendSpan(ExprAST& node, const sun::support::Position& start) const {
    sun::support::Position loc = start;
    loc.setEnd(prevTok_.end.line, prevTok_.end.column, prevTok_.end.offset);
    node.setLocation(std::move(loc));
  }

  /** Consumes tokens for a expression and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parseExpression();
  /** Consumes tokens for a unary operation and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parseUnary();
  /** Consumes tokens for a primary expression and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parsePrimary();
  /** Consumes tokens for a postfix expression and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parsePostfixExpr(unique_ptr<ExprAST> base);
  /** Consumes tokens for a variable declaration and builds its syntax-tree
   * representation. */
  unique_ptr<sun::ast::VariableCreationAST> parseVarStatement();
  /**
   * `var x = ...` or `const x = ...`, without the trailing semicolon
   */
  unique_ptr<sun::ast::VariableCreationAST> parseVarDeclaration();
  /**
   * `ref r = x;` (mutable) or, after `const`, `const ref r = x;`
   */
  unique_ptr<sun::ast::ReferenceCreationAST> parseRefStatement(
      sun::support::Position start, bool isMutable);
  /**
   * `const x = ...;` or `const ref r = x;`
   */
  unique_ptr<ExprAST> parseConstStatement();
  /**
   * Consumes tokens for a identifier expression and builds its syntax-tree
   * representation.
   */
  unique_ptr<ExprAST> parseIdentifierExpr();
  /**
   * Consumes tokens for a conditional statement and builds its syntax-tree
   * representation.
   */
  unique_ptr<sun::ast::IfExprAST> parseIfStatement();
  /** Consumes tokens for a pattern match and builds its syntax-tree
   * representation. */
  unique_ptr<sun::ast::MatchExprAST> parseMatchExpression();

  /**
   * Parsed match-arm pattern; body is attached later by parseMatchExpression
   */
  struct ParsedPattern {
    std::unique_ptr<ExprAST> pattern;  // null for wildcard
    bool isWildcard = false;
    bool hasPayloadParens = false;
    std::vector<sun::ast::PatternBinding> bindings;
    bool ok = false;
  };
  /** Consumes tokens for a match pattern and builds its syntax-tree
   * representation. */
  ParsedPattern parsePattern();
  /** Consumes tokens for a numeric literal and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parseNumberExpr();
  /** Consumes tokens for a character literal and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parseCharLiteral();
  /** Consumes tokens for a string literal and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parseStringLiteral();
  /** Consumes tokens for a array literal and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parseArrayLiteral();
  /**
   * Consumes tokens for a parenthesized expression and builds its syntax-tree
   * representation.
   */
  unique_ptr<ExprAST> parseParenExpr();
  /**
   * Consumes tokens for a binary-operation continuation and builds its
   * syntax-tree representation.
   */
  unique_ptr<ExprAST> parseBinOpRhs(int exprPrec, unique_ptr<ExprAST> lhs);
  /** Consumes tokens for a function signature and builds its syntax-tree
   * representation. */
  unique_ptr<sun::ast::PrototypeAST> parsePrototype();
  /** Consumes tokens for a function body and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parseFunctionLiteral(
      const std::string& name = "",
      std::vector<sun::ast::TypeParameter> typeParameters = {},
      bool isLambda = false, bool isLifecycleMethod = false,
      std::vector<sun::ast::LifetimeParameter> lifetimeParameters = {},
      bool isTestFunction = false);
  /** Consumes tokens for a function definition and builds its syntax-tree
   * representation. */
  unique_ptr<sun::ast::FunctionAST> parseFunction(bool isClassMethod = false,
                                                  bool isTest = false);
  /**
   * Parse a constructor or destructor member: init(args) { } / deinit() { }.
   * They are written without 'public' or 'method' and are always public.
   */
  unique_ptr<sun::ast::FunctionAST> parseLifecycleMethod();
  /**
   * True when the current token begins a fat-arrow lambda. The check restores
   * all parser state before returning, so parentheses and array literals stay
   * ordinary expressions when no lambda signature follows.
   */
  bool isLambdaLiteralStart();
  /** Consumes tokens for a lambda expression and builds its syntax-tree
   * representation. */
  unique_ptr<sun::ast::LambdaAST> parseLambda();
  /**
   * Parse an extern function or extern variable declaration.
   */
  unique_ptr<ExprAST> parseExtern();
  /**
   * Consumes tokens for a field initializer list and builds its syntax-tree
   * representation.
   */
  unique_ptr<sun::ast::StructLiteralAST> parseStructLiteral();
  /** Consumes tokens for a for loop and builds its syntax-tree representation.
   */
  unique_ptr<ExprAST> parseForLoop();  // Returns ForExprAST or ForInExprAST
  /** Consumes tokens for a while loop and builds its syntax-tree
   * representation. */
  unique_ptr<sun::ast::WhileExprAST> parseWhileLoop();
  /** Consumes tokens for a loop break and builds its syntax-tree
   * representation. */
  unique_ptr<sun::ast::BreakAST> parseBreak();
  /** Consumes tokens for a loop continuation and builds its syntax-tree
   * representation. */
  unique_ptr<sun::ast::ContinueAST> parseContinue();
  /** Consumes tokens for a string literal and builds its syntax-tree
   * representation. */
  unique_ptr<BlockExprAST> parseString(const std::string& source);
  /**
   * Every block records what construct it is the body of (see BlockKind);
   * only a match arm's or an unsafe block's body evaluates to a value.
   */
  unique_ptr<BlockExprAST> parseBlock(sun::ast::BlockKind kind,
                                      bool itemLevel = false);
  /** Consumes tokens for a statement and builds its syntax-tree representation.
   */
  unique_ptr<ExprAST> parseStatement();
  /** Consumes tokens for a statement and builds its syntax-tree representation.
   */
  unique_ptr<ExprAST> parseStatementCore();
  /**
   * Consumes an optional `public`; errors on a duplicate.
   */
  bool parsePublic();
  /**
   * Consumes an optional `const` before a class/interface member.
   */
  bool parseConstModifier();
  /**
   * True when the current token is the contextual `method` keyword. `method`
   * only starts a member of a class or interface; everywhere else it is an
   * ordinary identifier, so a field or variable may still be called `method`.
   */
  bool atMethodKeyword() const;
  /** Consumes tokens for a statement sequence and builds its syntax-tree
   * representation. */
  unique_ptr<ExprAST> parseStatementList();

  /**
   * `<T, U: _Numeric>` or `<'a, T>` after a function, class, interface or
   * enum name. Returns empty when there is no '<' — the declaration is not
   * generic. Each parameter may carry one constraint the type argument must
   * satisfy: a built-in trait such as `_Numeric`, or an interface name.
   * Lifetime parameters come first and land in lifetimesOut; passing
   * nullptr rejects them for declarations that do not accept lifetimes.
   */
  std::vector<sun::ast::TypeParameter> parseTypeParameterList(
      std::vector<sun::ast::LifetimeParameter>* lifetimesOut = nullptr);

  /**
   * Append dotted path segments after the first identifier of a name.
   */
  void parseQualifiedNameTail(std::string& name);

  /**
   * The constraint after the colon in `<T: _Numeric>`. Stamps the source span
   * the way parseTypeAnnotation does, so diagnostics can point at it.
   */
  sun::ast::TypeConstraint parseTypeConstraint(const std::string& paramName);

  /**
   * The trailing value pack in a parameter list: `args...`, or
   * `args...: _params_of<T>`. Called with the name already consumed and '...'
   * current. A pack ends the parameter list, so the caller stops after this.
   */
  sun::ast::VariadicParam parseVariadicParam(std::string name);

  /**
   * Type parsing. parseTypeAnnotation stamps the source span; the Impl
   * variant holds the grammar and leaves the span unset.
   */
  sun::ast::TypeAnnotation parseTypeAnnotation();
  /** Consumes tokens for a type annotation and builds its syntax-tree
   * representation. */
  sun::ast::TypeAnnotation parseTypeAnnotationImpl();
  /** Reports whether a token can start a type annotation. */
  bool isTypeToken(TokenKind kind);

  /**
   * Consumes tokens for a assignment or value expression and builds its
   * syntax-tree representation.
   */
  unique_ptr<ExprAST> parseAssignmentOrExpression();
  /** Parses the value assigned to an already parsed variable name. */
  unique_ptr<ExprAST> finishVariableAssignment(
      const std::string& name, const sun::support::Position& namePos);
  /** Parses an assignment to an already parsed member expression. */
  unique_ptr<ExprAST> finishMemberAssignment(unique_ptr<ExprAST> lhs);
  /** Parses an assignment to an already parsed indexed expression. */
  unique_ptr<ExprAST> finishIndexedAssignment(unique_ptr<ExprAST> expr);

  /**
   * Try-catch expression parsing: try { ... } catch (e: IError) { ... }
   */
  unique_ptr<ExprAST> parseTryCatch();

  /**
   * Unsafe block parsing: unsafe { ... }
   */
  unique_ptr<ExprAST> parseUnsafeBlock();

  /**
   * Throw expression parsing: throw &lt;expr&gt;
   */
  unique_ptr<ExprAST> parseThrow();

  /**
   * Class definition parsing: class Name { fields and methods }
   */
  unique_ptr<sun::ast::ClassDefinitionAST> parseClassDefinition();

  /**
   * Interface definition parsing: interface Name { fields and methods }
   */
  unique_ptr<sun::ast::InterfaceDefinitionAST> parseInterfaceDefinition();

  /**
   * Enum definition parsing: enum Name { Variant1, Variant2, ... }
   */
  unique_ptr<sun::ast::EnumDefinitionAST> parseEnumDefinition();

  /**
   * New class instance: new ClassName(args...)
   */
  unique_ptr<ExprAST> parseNewClassInstance(const std::string& className);

  /** Consumes tokens for a dependency manifest and builds its syntax-tree
   * representation. */
  unique_ptr<sun::ast::ManifestAST> parseManifest();
  /**
   * Consumes tokens for a source dependency list and builds its syntax-tree
   * representation.
   */
  std::vector<sun::ast::ManifestSunDependency> parseManifestSuns();
  /**
   * Consumes tokens for a compiled-library dependency list and builds its
   * syntax-tree representation.
   */
  std::vector<sun::ast::ManifestMoonDependency> parseManifestMoons();
  /**
   * Consumes tokens for a protobuf dependency list and builds its syntax-tree
   * representation.
   */
  std::vector<sun::ast::ManifestProtoDependency> parseManifestProtos();
  /**
   * Consumes tokens for a archive dependency list and builds its syntax-tree
   * representation.
   */
  std::vector<sun::ast::ManifestArchiveDependency> parseManifestArchives();
  /**
   * Consumes tokens for a target-specific dependency settings and builds its
   * syntax-tree representation.
   */
  std::vector<sun::ast::ManifestTargetBlock> parseManifestTargets();

  /**
   * Declare statement parsing:
   * - Forward function declaration: declare function name(args) RetType;
   * - Type declaration: declare [Alias =] Type<Args>;
   */
  unique_ptr<ExprAST> parseDeclareStatement();

  /**
   * Module declaration parsing: module Name { ... }
   */
  unique_ptr<sun::ast::ModuleAST> parseModuleDecl();

  /**
   * Using statement parsing: using Namespace::name; or using Namespace::*;
   */
  unique_ptr<sun::ast::UsingAST> parseUsingStatement();

  /**
   * Parse a qualified name: Namespace::name or Namespace::Nested::name
   */
  unique_ptr<ExprAST> parseQualifiedOrSimpleName();

  /**
   * Collect AST stubs from a precompiled .moon file
   * Returns a MoonScopeAST wrapping all module stubs with content hash
   * Returns nullptr if the moon was already imported
   */
  std::unique_ptr<sun::ast::MoonScopeAST> collectMoonImport(
      const sun::moon_bundling::MoonImport& moonImport);

  /**
   * Create AST stubs from module metadata and append to collectedAST
   * Used by both .moon imports and .sun metadata-driven imports
   */
  void createModuleStubs(const sun::moon::ModuleMetadata& metadata,
                         std::vector<std::unique_ptr<ExprAST>>& collectedAST);

  /**
   * Parse a type annotation from its string representation.
   */
  sun::ast::TypeAnnotation parseTypeFromString(const std::string& typeStr);

  /**
   * Setters for import resolution (used by Driver)
   */
  void setBaseDir(const std::string& dir) { baseDir = dir; }
  /** Updates the precompiled imports stored by this object. */
  void setPrecompiledImports(
      std::shared_ptr<std::vector<std::string>> imports) {
    precompiledImports = imports;
  }
  /**
   * Get the list of precompiled imports discovered during parsing
   */
  const std::vector<std::string>& getPrecompiledImports() const {
    return *precompiledImports;
  }

  /**
   * Get source text from lexer buffer (for storing generic method source)
   */
  std::string getSourceText(int startOffset, int endOffset) const {
    return lexer.getSourceText(startOffset, endOffset);
  }

  /**
   * Get current position offset
   */
  int getCurrentOffset() const { return curTok.start.offset; }

  /**
   * Check if string interpolation was used during parsing
   */
  bool usesStringInterpolation() const { return usesStringInterpolation_; }
  /** Records whether this string contains embedded expressions. */
  void setUsesStringInterpolation(bool value) {
    usesStringInterpolation_ = value;
  }

  /**
   * Opt in to collecting comments into the side table (off by default)
   */
  void setCollectComments(bool collect) {
    collectComments_ = collect;
    lexer.setEmitComments(collect);
  }
  /**
   * Collected comments, keyed by absolute start offset
   */
  const std::map<int, Comment>& getComments() const { return comments_; }
};
}  // namespace sun::parsing
