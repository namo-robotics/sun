#pragma once

#include <iostream>
#include <istream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "ast.h"
#include "ast/manifest_ast.h"
#include "ast_cache.h"
#include "error.h"
#include "lexer.h"
#include "moon/moon.h"
#include "moon_import.h"

using std::unique_ptr;

// The actual C function that prints a character
static double putchard(double X) {
  char c = static_cast<char>(X);
  putchar(c);
  fflush(stdout);  // Optional: ensure immediate output
  return 0.0;      // Kaleidoscope externs return double
}

// A source comment collected during parsing (never part of the AST; the
// formatter re-attaches comments to nodes by comparing spans)
struct Comment {
  Position span;     // start + end{Line,Column,Offset}
  std::string text;  // raw text including delimiters ("// x", "/* x */")
  bool ownLine;      // nothing but whitespace/comments precede it on its line
  bool isBlock;      // /* */ vs //
};

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

  static bool isCommentToken(const Token& tok) {
    return tok.kind == TokenKind::COMMENT ||
           tok.kind == TokenKind::BLOCK_COMMENT;
  }

  void recordComment(const Token& tok, int lastEndLine) {
    Position span = tok.start;
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

  // Helper: throw parsing error with source context
  [[noreturn]] void parsingError(const std::string& msg) {
    std::string sourceLine = lexer.getSourceLine(curTok.start.line);
    std::string prevLine =
        curTok.start.line > 1 ? lexer.getSourceLine(curTok.start.line - 1) : "";
    Position loc{curTok.start.line, curTok.start.column, curTok.start.offset,
                 currentFilePath.empty()
                     ? std::nullopt
                     : std::optional<std::string>(currentFilePath)};
    loc.setEnd(curTok.end.line, curTok.end.column, curTok.end.offset);
    logParsingError(loc, msg, sourceLine, prevLine);
  }

  // Helper: throw the error for a missing identifier. A keyword standing
  // where a name belongs is reported as such, since "expected identifier"
  // reads as a syntax problem rather than a name collision.
  [[noreturn]] void throwIdentifierError(const std::string& msg) {
    if (auto word = getKeywordSpelling(curTok.kind)) {
      parsingError("'" + std::string(*word) +
                   "' is a reserved word and cannot be used as an identifier");
    }
    parsingError(msg);
  }

  // Helper: expect current token to be an identifier, or throw error
  void expectIdentifier(const std::string& msg) {
    if (curTok.kind != TokenKind::IDENTIFIER) throwIdentifierError(msg);
  }

  // Helper: expect current token to be a specific kind, or throw error
  void expectCurrentTokenKind(TokenKind expected, const std::string& msg) {
    if (expected == TokenKind::IDENTIFIER) {
      expectIdentifier(msg);
      return;
    }
    if (curTok.kind != expected) {
      parsingError(msg);
    }
  }

  // Helper: consume a '>' token, handling '>>' split for nested generics
  // Returns true if consumed, throws error with msg if not
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
      TokenKind remainderKind = curTok.kind == TokenKind::RIGHT_SHIFT
                                    ? TokenKind::GREATER
                                : curTok.kind == TokenKind::GREATER_EQUAL
                                    ? TokenKind::EQUAL
                                    : TokenKind::GREATER_EQUAL;
      Position mid = curTok.start;
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

  // Helper: check if current token starts with '>' (for lookahead)
  bool isGreater() const {
    return curTok.kind == TokenKind::GREATER ||
           curTok.kind == TokenKind::RIGHT_SHIFT ||
           curTok.kind == TokenKind::GREATER_EQUAL ||
           curTok.kind == TokenKind::RIGHT_SHIFT_ASSIGN;
  }

 public:
  Parser() : lexer(std::cin) {}
  // Updated constructor: takes both input stream and codegen context
  Parser(std::istream& input) : lexer(input) {}

  // Set the file path for error messages
  void setFilePath(const std::string& path) { currentFilePath = path; }
  const std::string& getFilePath() const { return currentFilePath; }

  unique_ptr<BlockExprAST> parseProgram();
  // Convenience constructors (optional but recommended)

  static Parser createStringParser(const std::string& source) {
    auto* ss = new std::istringstream(source);
    Parser parser(*ss);
    parser.getNextToken();  // Prime the lexer
    return parser;
  }

  void pushToken(const Token& token) {
    tokenStack.push_back(curTok);
    curTok = token;
  }

  // Parsing functions
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

  // Start position of the node about to be parsed (curTok is its first token)
  Position captureStart() const {
    Position p = curTok.start;
    if (!currentFilePath.empty()) p.filePath = currentFilePath;
    return p;
  }

  // Move a node's span start back to `start` (e.g. over a leading modifier)
  static void extendSpanStart(ExprAST& node, Position start) {
    const Position& cur = node.getLocation();
    if (cur.hasEnd())
      start.setEnd(*cur.endLine, *cur.endColumn, cur.endOffset.value_or(0));
    node.setLocation(std::move(start));
  }

  // Stamp span [start, end-of-last-consumed-token] onto a finished node
  template <typename NodeT>
  unique_ptr<NodeT> finishNode(unique_ptr<NodeT> node, Position start) const {
    if (node) {
      start.setEnd(prevTok_.end.line, prevTok_.end.column,
                   prevTok_.end.offset);
      node->setLocation(std::move(start));
    }
    return node;
  }

  // Span variant for left-recursive constructs: start comes from an
  // already-stamped sub-node's location
  void extendSpan(ExprAST& node, const Position& start) const {
    Position loc = start;
    loc.setEnd(prevTok_.end.line, prevTok_.end.column, prevTok_.end.offset);
    node.setLocation(std::move(loc));
  }

  unique_ptr<ExprAST> parseExpression();
  unique_ptr<ExprAST> parseUnary();
  unique_ptr<ExprAST> parsePrimary();
  unique_ptr<ExprAST> parsePostfixExpr(unique_ptr<ExprAST> base);
  unique_ptr<VariableCreationAST> parseVarStatement();
  // `var x = ...` or `const x = ...`, without the trailing semicolon
  unique_ptr<VariableCreationAST> parseVarDeclaration();
  // `ref r = x;` (mutable) or, after `const`, `const ref r = x;`
  unique_ptr<ReferenceCreationAST> parseRefStatement(Position start,
                                                     bool isMutable);
  // `const x = ...;` or `const ref r = x;`
  unique_ptr<ExprAST> parseConstStatement();
  unique_ptr<ExprAST> parseIdentifierExpr();
  unique_ptr<IfExprAST> parseIfStatement();
  unique_ptr<MatchExprAST> parseMatchExpression();

  // Parsed match-arm pattern; body is attached later by parseMatchExpression
  struct ParsedPattern {
    std::unique_ptr<ExprAST> pattern;  // null for wildcard
    bool isWildcard = false;
    bool hasPayloadParens = false;
    std::vector<PatternBinding> bindings;
    bool ok = false;
  };
  ParsedPattern parsePattern();
  unique_ptr<ExprAST> parseNumberExpr();
  unique_ptr<ExprAST> parseStringLiteral();
  unique_ptr<ExprAST> parseArrayLiteral();
  unique_ptr<ExprAST> parseParenExpr();
  unique_ptr<ExprAST> parseBinOpRhs(int exprPrec, unique_ptr<ExprAST> lhs);
  unique_ptr<PrototypeAST> parsePrototype();
  unique_ptr<ExprAST> parseFunctionLiteral(
      const std::string& name = "",
      std::vector<std::string> typeParameters = {}, bool isLambda = false);
  unique_ptr<FunctionAST> parseFunction();
  unique_ptr<LambdaAST> parseLambda();
  unique_ptr<PrototypeAST> parseExtern();
  unique_ptr<StructLiteralAST> parseStructLiteral();
  unique_ptr<ExprAST> parseForLoop();  // Returns ForExprAST or ForInExprAST
  unique_ptr<WhileExprAST> parseWhileLoop();
  unique_ptr<BreakAST> parseBreak();
  unique_ptr<ContinueAST> parseContinue();
  unique_ptr<BlockExprAST> parseString(const std::string& source);
  unique_ptr<BlockExprAST> parseBlock(bool itemLevel = false);
  unique_ptr<ExprAST> parseStatement();
  unique_ptr<ExprAST> parseStatementCore();
  // Consumes an optional `public`; errors on a duplicate.
  bool parsePublic();
  // Consumes an optional `const` before a class/interface member.
  bool parseConstModifier();
  unique_ptr<ExprAST> parseStatementList();

  // Type parsing. parseTypeAnnotation stamps the source span; the Impl
  // variant holds the grammar and leaves the span unset.
  TypeAnnotation parseTypeAnnotation();
  TypeAnnotation parseTypeAnnotationImpl();
  bool isTypeToken(TokenKind kind);

  unique_ptr<ExprAST> parseAssignmentOrExpression();
  unique_ptr<ExprAST> finishVariableAssignment(const std::string& name,
                                               const Position& namePos);
  unique_ptr<ExprAST> finishMemberAssignment(unique_ptr<ExprAST> lhs);

  // Try-catch expression parsing: try { ... } catch (e: IError) { ... }
  unique_ptr<ExprAST> parseTryCatch();

  // Unsafe block parsing: unsafe { ... }
  unique_ptr<ExprAST> parseUnsafeBlock();

  // Throw expression parsing: throw <expr>
  unique_ptr<ExprAST> parseThrow();

  // Spawn expression parsing: spawn(lambda) - creates OS thread
  unique_ptr<ExprAST> parseSpawn();

  // Class definition parsing: class Name { fields and methods }
  unique_ptr<ClassDefinitionAST> parseClassDefinition();

  // Interface definition parsing: interface Name { fields and methods }
  unique_ptr<InterfaceDefinitionAST> parseInterfaceDefinition();

  // Enum definition parsing: enum Name { Variant1, Variant2, ... }
  unique_ptr<EnumDefinitionAST> parseEnumDefinition();

  // New class instance: new ClassName(args...)
  unique_ptr<ExprAST> parseNewClassInstance(const std::string& className);

  unique_ptr<ManifestAST> parseManifest();
  std::vector<ManifestSunDependency> parseManifestSuns();
  std::vector<ManifestMoonDependency> parseManifestMoons();
  std::vector<ManifestProtoDependency> parseManifestProtos();

  // Declare statement parsing:
  // - Forward function declaration: declare function name(args) RetType;
  // - Type declaration: declare [Alias =] Type<Args>;
  unique_ptr<ExprAST> parseDeclareStatement();

  // Module declaration parsing: module Name { ... }
  unique_ptr<ModuleAST> parseModuleDecl();

  // Using statement parsing: using Namespace::name; or using Namespace::*;
  unique_ptr<UsingAST> parseUsingStatement();

  // Parse a qualified name: Namespace::name or Namespace::Nested::name
  unique_ptr<ExprAST> parseQualifiedOrSimpleName();

  // Collect AST stubs from a precompiled .moon file
  // Returns a MoonScopeAST wrapping all module stubs with content hash
  // Returns nullptr if the moon was already imported
  std::unique_ptr<MoonScopeAST> collectMoonImport(
      const sun::MoonImport& moonImport);

  // Create AST stubs from module metadata and append to collectedAST
  // Used by both .moon imports and .sun metadata-driven imports
  void createModuleStubs(const sun::moon::ModuleMetadata& metadata,
                         std::vector<std::unique_ptr<ExprAST>>& collectedAST);

  // Parse a type annotation from its string representation.
  TypeAnnotation parseTypeFromString(const std::string& typeStr);

  // Setters for import resolution (used by Driver)
  void setBaseDir(const std::string& dir) { baseDir = dir; }
  void setPrecompiledImports(
      std::shared_ptr<std::vector<std::string>> imports) {
    precompiledImports = imports;
  }
  // Get the list of precompiled imports discovered during parsing
  const std::vector<std::string>& getPrecompiledImports() const {
    return *precompiledImports;
  }

  // Get source text from lexer buffer (for storing generic method source)
  std::string getSourceText(int startOffset, int endOffset) const {
    return lexer.getSourceText(startOffset, endOffset);
  }

  // Get current position offset
  int getCurrentOffset() const { return curTok.start.offset; }

  // Check if string interpolation was used during parsing
  bool usesStringInterpolation() const { return usesStringInterpolation_; }
  void setUsesStringInterpolation(bool value) {
    usesStringInterpolation_ = value;
  }

  // Opt in to collecting comments into the side table (off by default)
  void setCollectComments(bool collect) {
    collectComments_ = collect;
    lexer.setEmitComments(collect);
  }
  // Collected comments, keyed by absolute start offset
  const std::map<int, Comment>& getComments() const { return comments_; }
};