// interpolated_string_parser.cpp — Implementation of template string parsing

#include "interpolated_string_parser.h"

#include <sstream>
#include <stdexcept>

#include "ast.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"

std::unique_ptr<ExprAST> InterpolatedStringParser::parse(
    const std::string& content, const Position& location) {
  // Tokenize into segments (handles escapes, finds interpolations)
  auto segments = tokenize(content);

  // Build the desugared block that creates a String
  return buildDesugaredBlock(segments, location);
}

std::vector<InterpolatedSegment> InterpolatedStringParser::tokenize(
    const std::string& content) {
  std::vector<InterpolatedSegment> segments;
  size_t pos = 0;

  while (pos < content.size()) {
    // Look for ${
    size_t interpStart = content.find("${", pos);

    // Check for escaped $
    while (interpStart != std::string::npos && interpStart > 0 &&
           content[interpStart - 1] == '\\') {
      // This is \${, skip it and look for next
      interpStart = content.find("${", interpStart + 2);
    }

    if (interpStart == std::string::npos) {
      // No more interpolations - rest is literal
      std::string literal = content.substr(pos);
      if (!literal.empty()) {
        segments.push_back(
            InterpolatedSegment::makeLiteral(processEscapes(literal)));
      }
      break;
    }

    // Literal before ${
    if (interpStart > pos) {
      std::string literal = content.substr(pos, interpStart - pos);
      segments.push_back(
          InterpolatedSegment::makeLiteral(processEscapes(literal)));
    }

    // Find matching }
    size_t exprStart = interpStart + 2;
    size_t exprEnd = findMatchingBrace(content, exprStart);

    if (exprEnd == std::string::npos) {
      logAndThrowError("Unterminated interpolation expression: missing '}'");
    }

    // Extract and parse the expression
    std::string exprText = content.substr(exprStart, exprEnd - exprStart);
    auto expr = parseExpression(exprText);

    if (expr) {
      segments.push_back(InterpolatedSegment::makeExpression(std::move(expr)));
    }

    pos = exprEnd + 1;  // Skip past the }
  }

  return segments;
}

std::string InterpolatedStringParser::processEscapes(const std::string& raw) {
  std::string result;
  result.reserve(raw.size());

  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      char next = raw[i + 1];
      switch (next) {
        case '`':
          result += '`';
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
        case '$':
          result += '$';
          break;  // Escape ${
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

size_t InterpolatedStringParser::findMatchingBrace(const std::string& content,
                                                   size_t start) {
  int depth = 1;
  size_t pos = start;

  while (pos < content.size() && depth > 0) {
    char c = content[pos];

    // Skip string literals to avoid counting braces inside them
    if (c == '"') {
      pos++;
      while (pos < content.size() && content[pos] != '"') {
        if (content[pos] == '\\' && pos + 1 < content.size()) {
          pos++;  // Skip escaped char
        }
        pos++;
      }
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) {
        return pos;
      }
    }
    pos++;
  }

  return std::string::npos;
}

std::unique_ptr<ExprAST> InterpolatedStringParser::parseExpression(
    const std::string& exprText) {
  if (exprText.empty()) {
    logAndThrowError("Empty interpolation expression");
  }

  // Create a sub-parser using the factory method that primes the lexer
  Parser subParser = Parser::createStringParser(exprText);

  // Parse a single expression
  auto expr = subParser.parseExpression();

  return expr;
}

std::unique_ptr<BlockExprAST> InterpolatedStringParser::buildDesugaredBlock(
    std::vector<InterpolatedSegment>& segments, const Position& location) {
  auto block = std::make_unique<BlockExprAST>();

  // var interp_alloc_ = sun.HeapAllocator();
  auto allocCall =
      makeCall(makeMemberAccess(makeVarRef("sun"), "HeapAllocator"), {});
  block->addExpression(makeVarCreate("interp_alloc_", std::move(allocCall)));

  // var interp_result_ = sun.String(interp_alloc_, "");
  std::vector<std::unique_ptr<ExprAST>> stringArgs;
  stringArgs.push_back(makeVarRef("interp_alloc_"));
  stringArgs.push_back(makeStringLiteral(""));
  auto stringCall = makeCall(makeMemberAccess(makeVarRef("sun"), "String"),
                             std::move(stringArgs));
  block->addExpression(makeVarCreate("interp_result_", std::move(stringCall)));

  // For each segment, emit interp_result_.append*(...)
  for (auto& segment : segments) {
    std::vector<std::unique_ptr<ExprAST>> appendArgs;
    std::string methodName;

    if (segment.isLiteral) {
      if (segment.literalText.empty()) {
        continue;  // Skip empty literals
      }
      appendArgs.push_back(makeStringLiteral(segment.literalText));
      methodName = "append_literal";  // Use specific method for literals
    } else {
      appendArgs.push_back(std::move(segment.expression));
      methodName = "append";  // Use overloaded append for expressions
    }

    auto appendCall =
        makeCall(makeMemberAccess(makeVarRef("interp_result_"), methodName),
                 std::move(appendArgs));
    block->addExpression(std::move(appendCall));
  }

  // Final expression: interp_result_ (block returns this)
  block->addExpression(makeVarRef("interp_result_"));

  return block;
}

// Helper implementations

std::unique_ptr<VariableReferenceAST> InterpolatedStringParser::makeVarRef(
    const std::string& name) {
  return std::make_unique<VariableReferenceAST>(name);
}

std::unique_ptr<StringLiteralAST> InterpolatedStringParser::makeStringLiteral(
    const std::string& value) {
  return std::make_unique<StringLiteralAST>(value);
}

std::unique_ptr<MemberAccessAST> InterpolatedStringParser::makeMemberAccess(
    std::unique_ptr<ExprAST> object, const std::string& member) {
  return std::make_unique<MemberAccessAST>(std::move(object), member);
}

std::unique_ptr<CallExprAST> InterpolatedStringParser::makeCall(
    std::unique_ptr<ExprAST> callee,
    std::vector<std::unique_ptr<ExprAST>> args) {
  return std::make_unique<CallExprAST>(std::move(callee), std::move(args));
}

std::unique_ptr<VariableCreationAST> InterpolatedStringParser::makeVarCreate(
    const std::string& name, std::unique_ptr<ExprAST> value) {
  return std::make_unique<VariableCreationAST>(name, std::move(value));
}
