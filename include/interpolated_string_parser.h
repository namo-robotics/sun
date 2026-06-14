// interpolated_string_parser.h — Parser for template string interpolation
//
// Handles parsing of template strings like `Hello ${name}, age ${age}!`
// Desugars into a block expression with String construction and append() calls.

#pragma once

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "position.h"

// Forward declaration
class Parser;

// Segment of an interpolated string: either a literal or an expression
struct InterpolatedSegment {
  bool isLiteral;
  std::string literalText;              // If isLiteral is true
  std::unique_ptr<ExprAST> expression;  // If isLiteral is false

  static InterpolatedSegment makeLiteral(std::string text) {
    return {true, std::move(text), nullptr};
  }

  static InterpolatedSegment makeExpression(std::unique_ptr<ExprAST> expr) {
    return {false, "", std::move(expr)};
  }
};

// Parser for interpolated template strings
// Converts `Hello ${name}!` into:
// {
//   var __interp_alloc = sun.HeapAllocator();
//   var __interp_result = sun.String(__interp_alloc, "");
//   __interp_result.append("Hello ");
//   __interp_result.append(name);
//   __interp_result.append("!");
//   __interp_result
// }
class InterpolatedStringParser {
 public:
  // Parse a template string (content without backticks)
  // Returns the desugared BlockExprAST, or a simple StringLiteralAST if no
  // interpolation
  static std::unique_ptr<ExprAST> parse(const std::string& content,
                                        const Position& location);

 private:
  // Split the template string into segments (alternating literals and
  // expressions)
  static std::vector<InterpolatedSegment> tokenize(const std::string& content);

  // Process escape sequences in a literal segment
  static std::string processEscapes(const std::string& raw);

  // Find the matching closing brace for an expression, accounting for nesting
  static size_t findMatchingBrace(const std::string& content, size_t start);

  // Parse an expression string using a sub-parser
  static std::unique_ptr<ExprAST> parseExpression(const std::string& exprText);

  // Build the desugared block expression from segments
  static std::unique_ptr<BlockExprAST> buildDesugaredBlock(
      std::vector<InterpolatedSegment>& segments, const Position& location);

  // Helper: create variable reference AST
  static std::unique_ptr<VariableReferenceAST> makeVarRef(
      const std::string& name);

  // Helper: create string literal AST
  static std::unique_ptr<StringLiteralAST> makeStringLiteral(
      const std::string& value);

  // Helper: create member access AST (obj.member)
  static std::unique_ptr<MemberAccessAST> makeMemberAccess(
      std::unique_ptr<ExprAST> object, const std::string& member);

  // Helper: create call expression AST
  static std::unique_ptr<CallExprAST> makeCall(
      std::unique_ptr<ExprAST> callee,
      std::vector<std::unique_ptr<ExprAST>> args);

  // Helper: create variable creation AST
  static std::unique_ptr<VariableCreationAST> makeVarCreate(
      const std::string& name, std::unique_ptr<ExprAST> value);
};
