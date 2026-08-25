// name_ranges.cpp — Narrowing a declaration's span to the name written in it

#include "lsp/name_ranges.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include "ast.h"

namespace sun::lsp {

namespace {

bool isIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The signature text of a function or lambda: the prototype span, or the
// node up to its body when the prototype has none
std::optional<Position> signatureSpan(const ExprAST& owner) {
  const PrototypeAST* proto = prototypeOf(owner);
  if (!proto) return std::nullopt;
  Position span = proto->getLocation();
  if (span.endOffset) return span;
  if (owner.getType() != ASTNodeType::LAMBDA) return std::nullopt;
  span = owner.getLocation();
  span.endOffset =
      static_cast<const LambdaAST&>(owner).getBody().getLocation().offset;
  return span;
}

}  // namespace

int findWord(const std::string& text, const std::string& name, size_t from,
             size_t to) {
  if (name.empty() || from >= text.size()) return -1;
  if (to > text.size()) to = text.size();
  size_t pos = text.find(name, from);
  while (pos != std::string::npos && pos + name.size() <= to) {
    bool startsWord = pos == 0 || !isIdentifierChar(text[pos - 1]);
    bool endsWord = pos + name.size() == text.size() ||
                    !isIdentifierChar(text[pos + name.size()]);
    if (startsWord && endsWord) return static_cast<int>(pos);
    pos = text.find(name, pos + 1);
  }
  return -1;
}

Position rangeAt(const Position& base, int offset, int length) {
  Position range = base;
  range.offset = offset;
  range.endOffset = offset + length;
  return range;
}

bool textHas(const std::string& text, int offset, const std::string& word) {
  return offset >= 0 &&
         static_cast<size_t>(offset) + word.size() <= text.size() &&
         text.compare(offset, word.size(), word) == 0;
}

Position nameRange(const Position& span, const std::string& name,
                   const std::string& text) {
  if (name.empty()) {
    Position range = span;
    if (!range.endOffset) range.endOffset = range.offset;
    return range;
  }
  int length = static_cast<int>(name.size());
  if (span.endOffset) {
    if (sliceSpan(text, span) == name) return span;
    int at = findWord(text, name, span.offset, *span.endOffset);
    if (at >= 0) return rangeAt(span, at, length);
  } else if (textHas(text, span.offset, name)) {
    return rangeAt(span, span.offset, length);
  }
  if (textHas(text, span.offset - length, name)) {
    return rangeAt(span, span.offset - length, length);
  }
  return rangeAt(span, span.offset, 0);
}

const PrototypeAST* prototypeOf(const ExprAST& node) {
  if (node.getType() == ASTNodeType::FUNCTION) {
    return &static_cast<const FunctionAST&>(node).getProto();
  }
  if (node.getType() == ASTNodeType::LAMBDA) {
    return &static_cast<const LambdaAST&>(node).getProto();
  }
  return nullptr;
}

bool declaresParameter(const PrototypeAST& proto, const std::string& name) {
  for (const auto& arg : proto.getArgs()) {
    if (arg.first == name) return true;
  }
  return proto.hasVariadicParam() && *proto.getVariadicParamName() == name;
}

std::optional<Position> parameterRange(const ExprAST& owner,
                                       const std::string& name,
                                       const std::string& text) {
  std::optional<Position> span = signatureSpan(owner);
  if (!span || !span->endOffset) return std::nullopt;
  size_t from = text.find('(', span->offset);
  if (from == std::string::npos || static_cast<int>(from) >= *span->endOffset) {
    return std::nullopt;
  }
  for (int at = findWord(text, name, from, *span->endOffset); at >= 0;
       at = findWord(text, name, at + 1, *span->endOffset)) {
    size_t after = at + name.size();
    while (after < text.size() &&
           std::isspace(static_cast<unsigned char>(text[after]))) {
      ++after;
    }
    if (after < text.size() &&
        (text[after] == ':' || text.compare(after, 3, "...") == 0)) {
      return rangeAt(*span, at, static_cast<int>(name.size()));
    }
  }
  return std::nullopt;
}

std::optional<std::string> textOf(const Position& location,
                                  const std::string& documentPath,
                                  const std::string& source) {
  std::string text = sourceFor(location, documentPath, source);
  if (!text.empty()) return text;
  if (!location.filePath) return std::nullopt;
  std::ifstream file(*location.filePath);
  if (!file.is_open()) return std::nullopt;
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

Position nameRangeOf(const Declaration& declaration, const std::string& text) {
  const PrototypeAST* proto =
      declaration.node ? prototypeOf(*declaration.node) : nullptr;
  if (proto && (declaration.node->getType() == ASTNodeType::LAMBDA ||
                declaration.name != proto->getName())) {
    if (auto parameter =
            parameterRange(*declaration.node, declaration.name, text)) {
      return *parameter;
    }
  }
  return nameRange(declaration.location, declaration.name, text);
}

SymbolLocation makeSymbolLocation(const std::string& filePath,
                                  const Position& range,
                                  const std::string& text) {
  SymbolLocation location;
  location.filePath = filePath;
  location.range = range;
  location.start = lspPositionFromByteOffset(text, range.offset);
  location.end =
      lspPositionFromByteOffset(text, range.endOffset.value_or(range.offset));
  return location;
}

}  // namespace sun::lsp
