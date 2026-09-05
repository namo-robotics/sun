// interpolated_string_parser.cpp — Implementation of template string parsing

#include "parsing/interpolated_string_parser.h"

#include <sstream>
#include <stdexcept>

#include "ast.h"
#include "parsing/escapes.h"
#include "parsing/lexer.h"
#include "parsing/parser.h"
#include "support/error.h"

std::unique_ptr<InterpolatedStringAST> InterpolatedStringParser::parseToAst(
    const std::string& content, const Position& start, const Position& end,
    const std::string& filePath, sun::SourceFileId sourceFile) {
  auto segments = tokenize(content, start, filePath, sourceFile);

  auto node =
      std::make_unique<InterpolatedStringAST>(content, std::move(segments));
  Position loc = start;
  if (!filePath.empty()) loc.filePath = filePath;
  loc.setEnd(end.line, end.column, end.offset);
  node->setLocation(std::move(loc));
  node->inheritSourceFile(sourceFile);
  return node;
}

std::vector<InterpolatedStringAST::Segment> InterpolatedStringParser::tokenize(
    const std::string& content, const Position& start,
    const std::string& filePath, sun::SourceFileId sourceFile) {
  std::vector<InterpolatedStringAST::Segment> segments;
  // Content begins one byte past the opening backtick
  const int contentOffset = start.offset + 1;
  size_t pos = 0;

  // Line/column of a content-relative index, derived from the token start
  auto positionAt = [&](size_t idx) {
    int line = start.line;
    int col = start.column + 1;  // +1 for the opening backtick
    for (size_t i = 0; i < idx; ++i) {
      if (content[i] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
    return std::pair<int, int>{line, col};
  };

  auto makeLiteral = [&](size_t from, size_t len) {
    InterpolatedStringAST::Segment seg;
    seg.isLiteral = true;
    seg.rawText = content.substr(from, len);
    seg.cookedText = processEscapes(seg.rawText);
    seg.sourceOffset = contentOffset + static_cast<int>(from);
    segments.push_back(std::move(seg));
  };

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
      if (pos < content.size()) {
        makeLiteral(pos, content.size() - pos);
      }
      break;
    }

    // Literal before ${
    if (interpStart > pos) {
      makeLiteral(pos, interpStart - pos);
    }

    // Find matching }
    size_t exprStart = interpStart + 2;
    size_t exprEnd = findMatchingBrace(content, exprStart);

    if (exprEnd == std::string::npos) {
      logAndThrowError("Unterminated interpolation expression: missing '}'");
    }

    // Extract and parse the expression
    std::string exprText = content.substr(exprStart, exprEnd - exprStart);
    auto expr = parseExpression(exprText, sourceFile);

    if (expr) {
      auto [lineBase, colBase] = positionAt(exprStart);
      rebasePositions(*expr, lineBase, colBase,
                      contentOffset + static_cast<int>(exprStart), filePath);

      InterpolatedStringAST::Segment seg;
      seg.isLiteral = false;
      seg.rawText = std::move(exprText);
      seg.expression = std::move(expr);
      seg.sourceOffset = contentOffset + static_cast<int>(interpStart);
      segments.push_back(std::move(seg));
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
      if (next == '`' || next == '$') {
        result += next;  // \` and \$ are the template-specific escapes
      } else if (auto c = sun::escapes::simple(next)) {
        result += *c;
      } else {
        // Unknown escape - keep as-is
        result += raw[i];
        result += next;
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
    const std::string& exprText, sun::SourceFileId sourceFile) {
  if (exprText.empty()) {
    logAndThrowError("Empty interpolation expression");
  }

  // Create a sub-parser using the factory method that primes the lexer
  Parser subParser = Parser::createStringParser(exprText);
  subParser.setSourceFileId(sourceFile);

  // Parse a single expression
  auto expr = subParser.parseExpression();

  return expr;
}

void InterpolatedStringParser::rebasePositions(ExprAST& expr, int lineBase,
                                               int colBase, int offsetBase,
                                               const std::string& filePath) {
  Position loc = expr.getLocation();
  // Fragment positions are 1-based relative to the fragment start
  if (loc.line == 1) loc.column += colBase - 1;
  loc.line += lineBase - 1;
  loc.offset += offsetBase;
  if (loc.endOffset) *loc.endOffset += offsetBase;
  if (loc.endLine) {
    if (*loc.endLine == 1 && loc.endColumn) *loc.endColumn += colBase - 1;
    *loc.endLine += lineBase - 1;
  }
  if (!filePath.empty()) loc.filePath = filePath;
  expr.setLocation(std::move(loc));

  expr.forEachChildSlot([&](std::unique_ptr<ExprAST>& child) {
    if (child) rebasePositions(*child, lineBase, colBase, offsetBase, filePath);
  });
}

std::unique_ptr<BlockExprAST> InterpolatedStringParser::desugar(
    InterpolatedStringAST& node) {
  const Position& loc = node.getLocation();
  auto block = std::make_unique<BlockExprAST>();
  // A Value block: the one block kind with no syntax, so the lowering can
  // hand back the built String as the block's value
  block->setKind(BlockKind::Value);
  block->setLocation(loc);

  // var interp_alloc_ = std.HeapAllocator();
  auto allocCall = makeCall(
      makeMemberAccess(makeVarRef("std", loc), "HeapAllocator", loc), {}, loc);
  block->addExpression(
      makeVarCreate("interp_alloc_", std::move(allocCall), loc));

  // var interp_result_ = std.String(interp_alloc_, "");
  std::vector<std::unique_ptr<ExprAST>> stringArgs;
  stringArgs.push_back(makeVarRef("interp_alloc_", loc));
  stringArgs.push_back(makeStringLiteral("", loc));
  auto stringCall =
      makeCall(makeMemberAccess(makeVarRef("std", loc), "String", loc),
               std::move(stringArgs), loc);
  block->addExpression(
      makeVarCreate("interp_result_", std::move(stringCall), loc));

  // interp_result_.reserve(n)
  //
  // The literal segments' total size is known here, so the buffer can be
  // sized once instead of doubling its way there one append at a time.
  // Interpolated values are not counted: their length is a runtime property,
  // and guessing it made short values slower by forcing an allocation the
  // default capacity would have covered. Under-reserving simply grows as
  // before. The 16 of slack mirrors what String's own constructor adds, and
  // is also the floor below which a reserve call would achieve nothing.
  size_t literalBytes = 0;
  for (const auto& segment : node.getSegments()) {
    if (segment.isLiteral) literalBytes += segment.cookedText.size();
  }
  if (literalBytes > 16) {
    std::vector<std::unique_ptr<ExprAST>> reserveArgs;
    reserveArgs.push_back(
        makeNumberLiteral(static_cast<int64_t>(literalBytes + 16), loc));
    block->addExpression(makeCall(
        makeMemberAccess(makeVarRef("interp_result_", loc), "reserve", loc),
        std::move(reserveArgs), loc));
  }

  // For each segment, emit interp_result_.append*(...)
  for (auto& segment : node.getSegmentsMutable()) {
    std::vector<std::unique_ptr<ExprAST>> appendArgs;
    std::string methodName;

    if (segment.isLiteral) {
      if (segment.cookedText.empty()) {
        continue;  // Skip empty literals
      }
      appendArgs.push_back(makeStringLiteral(segment.cookedText, loc));
      methodName = "append_literal";  // Use specific method for literals
    } else {
      if (!segment.expression) continue;
      appendArgs.push_back(std::move(segment.expression));
      methodName = "append";  // Use overloaded append for expressions
    }

    auto appendCall = makeCall(
        makeMemberAccess(makeVarRef("interp_result_", loc), methodName, loc),
        std::move(appendArgs), loc);
    block->addExpression(std::move(appendCall));
  }

  // Final expression: interp_result_ (block returns this)
  block->addExpression(makeVarRef("interp_result_", loc));

  return block;
}

// Helper implementations

std::unique_ptr<VariableReferenceAST> InterpolatedStringParser::makeVarRef(
    const std::string& name, const Position& loc) {
  auto node = std::make_unique<VariableReferenceAST>(name);
  node->setLocation(loc);
  return node;
}

std::unique_ptr<NumberExprAST> InterpolatedStringParser::makeNumberLiteral(
    int64_t value, const Position& loc) {
  auto node = std::make_unique<NumberExprAST>(value);
  node->setLocation(loc);
  return node;
}

std::unique_ptr<StringLiteralAST> InterpolatedStringParser::makeStringLiteral(
    const std::string& value, const Position& loc) {
  auto node = std::make_unique<StringLiteralAST>(value);
  node->setLocation(loc);
  return node;
}

std::unique_ptr<MemberAccessAST> InterpolatedStringParser::makeMemberAccess(
    std::unique_ptr<ExprAST> object, const std::string& member,
    const Position& loc) {
  auto node = std::make_unique<MemberAccessAST>(std::move(object), member);
  node->setLocation(loc);
  return node;
}

std::unique_ptr<CallExprAST> InterpolatedStringParser::makeCall(
    std::unique_ptr<ExprAST> callee, std::vector<std::unique_ptr<ExprAST>> args,
    const Position& loc) {
  auto node = std::make_unique<CallExprAST>(std::move(callee), std::move(args));
  node->setLocation(loc);
  return node;
}

std::unique_ptr<VariableCreationAST> InterpolatedStringParser::makeVarCreate(
    const std::string& name, std::unique_ptr<ExprAST> value,
    const Position& loc) {
  auto node = std::make_unique<VariableCreationAST>(name, std::move(value));
  node->setLocation(loc);
  return node;
}
