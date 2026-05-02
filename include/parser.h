#pragma once

#include <iostream>
#include <istream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "ast.h"
#include "error.h"
#include "lexer.h"
#include "moon.h"

using std::unique_ptr;

// The actual C function that prints a character
static double putchard(double X) {
  char c = static_cast<char>(X);
  putchar(c);
  fflush(stdout);  // Optional: ensure immediate output
  return 0.0;      // Kaleidoscope externs return double
}

class Parser {
 private:
  Lexer lexer;
  Token curTok = Token::eof({0, 0, 0});
  std::vector<Token> tokenStack;

  // Track which files have already been imported (for cycle detection)
  // Shared across recursive import calls
  std::shared_ptr<std::set<std::string>> importedFiles =
      std::make_shared<std::set<std::string>>();

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
    logParsingError(loc, msg, sourceLine, prevLine);
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
    if (!tokenStack.empty()) {
      curTok = tokenStack.back();
      tokenStack.pop_back();
      return curTok;
    }
    curTok = lexer.getNextToken();
    return curTok;
  }

  unique_ptr<ExprAST> parseExpression();
  unique_ptr<ExprAST> parsePrimary();
  unique_ptr<ExprAST> parsePostfixExpr(unique_ptr<ExprAST> base);
  unique_ptr<VariableCreationAST> parseVarStatement();
  unique_ptr<VariableCreationAST>
  parseVarDeclaration();  // Without trailing semicolon
  unique_ptr<ReferenceCreationAST> parseRefStatement();
  unique_ptr<ExprAST> parseIdentifierExpr();
  unique_ptr<IfExprAST> parseIfStatement();
  unique_ptr<MatchExprAST> parseMatchExpression();
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
  unique_ptr<ExprAST> parseForLoop();  // Returns ForExprAST or ForInExprAST
  unique_ptr<WhileExprAST> parseWhileLoop();
  unique_ptr<BreakAST> parseBreak();
  unique_ptr<ContinueAST> parseContinue();
  unique_ptr<BlockExprAST> parseString(const std::string& source);
  unique_ptr<BlockExprAST> parseBlock();
  unique_ptr<ExprAST> parseStatement();
  unique_ptr<ExprAST> parseStatementList();

  // Type parsing
  TypeAnnotation parseTypeAnnotation();
  bool isTypeToken(TokenKind kind);

  unique_ptr<ExprAST> parseAssignmentOrExpression();

  // Try-catch expression parsing: try { ... } catch (e: IError) { ... }
  unique_ptr<ExprAST> parseTryCatch();

  // Throw expression parsing: throw <expr>
  unique_ptr<ExprAST> parseThrow();

  // Class definition parsing: class Name { fields and methods }
  unique_ptr<ClassDefinitionAST> parseClassDefinition();

  // Interface definition parsing: interface Name { fields and methods }
  unique_ptr<InterfaceDefinitionAST> parseInterfaceDefinition();

  // Enum definition parsing: enum Name { Variant1, Variant2, ... }
  unique_ptr<EnumDefinitionAST> parseEnumDefinition();

  // New class instance: new ClassName(args...)
  unique_ptr<ExprAST> parseNewClassInstance(const std::string& className);

  // Import statement parsing: import "file.sun";
  unique_ptr<ImportAST> parseImportStatement();

  // Declare type statement parsing: declare [Alias =] Type<Args>;
  unique_ptr<DeclareTypeAST> parseDeclareStatement();

  // Namespace declaration parsing: namespace Name { ... }
  unique_ptr<NamespaceAST> parseNamespaceDecl();

  // Using statement parsing: using Namespace::name; or using Namespace::*;
  unique_ptr<UsingAST> parseUsingStatement();

  // Parse a qualified name: Namespace::name or Namespace::Nested::name
  unique_ptr<ExprAST> parseQualifiedOrSimpleName();

  // Expand an import into an ImportScopeAST (non-transitive import system)
  // cycleStack tracks ancestors on the current import path to detect cycles.
  // precompiledImports is shared for .moon registration.
  std::unique_ptr<ImportScopeAST> expandImport(
      const std::string& importPath, std::set<std::string>& cycleStack);

  // Collect AST stubs from a precompiled .moon file
  void collectMoonImport(const std::string& moonPath,
                         std::vector<std::unique_ptr<ExprAST>>& collectedAST);

  // Create AST stubs from module metadata and append to collectedAST
  // Used by both .moon imports and .sun metadata-driven imports
  void createModuleStubs(const sun::ModuleMetadata& metadata,
                         std::vector<std::unique_ptr<ExprAST>>& collectedAST);

  // Parse a type annotation from its string representation.
  TypeAnnotation parseTypeFromString(const std::string& typeStr);

  // Parse a function signature string (e.g., "(i32, i32) -> i32") into an
  // extern FunctionAST.
  std::unique_ptr<FunctionAST> parseFunctionSignature(
      const std::string& name, const std::string& signature);

  // Parse a method from its source text (for loading from moon)
  std::unique_ptr<FunctionAST> parseFunctionFromSource(
      const std::string& source);

  // Static lazy parsing helper - uses thread-local Parser to avoid NFA rebuild
  // Call from codegen when a method body needs on-demand parsing
  static std::unique_ptr<FunctionAST> lazyParseFunctionSource(
      const std::string& source);

  // Setters for import resolution (used by Driver)
  void setBaseDir(const std::string& dir) { baseDir = dir; }
  void setImportedFiles(std::shared_ptr<std::set<std::string>> files) {
    importedFiles = files;
  }
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
};