// interpolated_string_parser.h — Parser for template string interpolation
//
// Parses template strings like `Hello ${name}, age ${age}!` into an
// InterpolatedStringAST (lossless parse tree). The lowering pass later calls
// desugar() to turn the node into a block expression with String construction
// and append() calls.

#pragma once

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "support/position.h"

// Forward declaration
class Parser;

// Parser for interpolated template strings.
// parseToAst: `Hello ${name}!` -> InterpolatedStringAST (kept in parse tree)
// desugar (called by LoweringPass):
// {
//   var interp_alloc_ = sun.HeapAllocator();
//   var interp_result_ = sun.String(interp_alloc_, "");
//   interp_result_.append_literal("Hello ");
//   interp_result_.append(name);
//   interp_result_.append_literal("!");
//   interp_result_
// }
class InterpolatedStringParser {
 public:
  // Parse a template string token into the lossless AST node.
  // content: inner text without backticks (escapes unprocessed).
  // start/end: the TEMPLATE_STRING token's span (backticks included).
  // Sub-expression positions inside ${...} are rebased to absolute file
  // positions (best-effort; exact for offsets).
  static std::unique_ptr<InterpolatedStringAST> parseToAst(
      const std::string& content, const Position& start, const Position& end,
      const std::string& filePath);

  // Desugar the node into the runtime block (consumes segment expressions).
  // Synthetic nodes are stamped with the template's location.
  static std::unique_ptr<BlockExprAST> desugar(InterpolatedStringAST& node);

 private:
  // Split the template string into segments (alternating literals and
  // expressions). start = template token start (for position rebasing).
  static std::vector<InterpolatedStringAST::Segment> tokenize(
      const std::string& content, const Position& start,
      const std::string& filePath);

  // Process escape sequences in a literal segment
  static std::string processEscapes(const std::string& raw);

  // Find the matching closing brace for an expression, accounting for nesting
  static size_t findMatchingBrace(const std::string& content, size_t start);

  // Parse an expression string using a sub-parser
  static std::unique_ptr<ExprAST> parseExpression(const std::string& exprText);

  // Rebase fragment-relative positions in a sub-expression tree to absolute
  // file positions
  static void rebasePositions(ExprAST& expr, int lineBase, int colBase,
                              int offsetBase, const std::string& filePath);

  // Helper: create variable reference AST
  static std::unique_ptr<VariableReferenceAST> makeVarRef(
      const std::string& name, const Position& loc);

  // Helper: create string literal AST
  static std::unique_ptr<NumberExprAST> makeNumberLiteral(int64_t value,
                                                          const Position& loc);
  static std::unique_ptr<StringLiteralAST> makeStringLiteral(
      const std::string& value, const Position& loc);

  // Helper: create member access AST (obj.member)
  static std::unique_ptr<MemberAccessAST> makeMemberAccess(
      std::unique_ptr<ExprAST> object, const std::string& member,
      const Position& loc);

  // Helper: create call expression AST
  static std::unique_ptr<CallExprAST> makeCall(
      std::unique_ptr<ExprAST> callee,
      std::vector<std::unique_ptr<ExprAST>> args, const Position& loc);

  // Helper: create variable creation AST
  static std::unique_ptr<VariableCreationAST> makeVarCreate(
      const std::string& name, std::unique_ptr<ExprAST> value,
      const Position& loc);
};
