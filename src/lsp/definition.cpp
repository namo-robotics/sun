// definition.cpp — Go-to-definition for the language server

#include "lsp/definition.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include "ast.h"
#include "lsp/declarations.h"

namespace sun::lsp {

namespace {

bool isIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Offset of the first whole word `name` in text[from, to), or -1
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

// `length` bytes at `offset` in the file of `base`
Position rangeAt(const Position& base, int offset, int length) {
  Position range = base;
  range.offset = offset;
  range.endOffset = offset + length;
  return range;
}

bool textHas(const std::string& text, int offset, const std::string& word) {
  return offset >= 0 && static_cast<size_t>(offset) + word.size() <= text.size() &&
         text.compare(offset, word.size(), word) == 0;
}

// The name token of a declaration: the span itself when it already is the
// name, otherwise the first whole word `name` inside it, or the name just
// before it (a signature span starts at its parenthesis). Falls back to an
// empty range at the start of the span.
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

// The signature text of a function or lambda: the prototype span, or the
// node up to its body when the prototype has none
std::optional<Position> signatureSpan(const ExprAST& owner) {
  const PrototypeAST* proto = prototypeOf(owner);
  if (!proto) return std::nullopt;
  Position span = proto->getLocation();
  if (span.endOffset) return span;
  if (owner.getType() != ASTNodeType::LAMBDA) return std::nullopt;
  span = owner.getLocation();
  span.endOffset = static_cast<const LambdaAST&>(owner)
                       .getBody()
                       .getLocation()
                       .offset;
  return span;
}

// Where `name` is declared in a signature: `name:` or `name...` after the
// opening parenthesis
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

// The nearest enclosing function or lambda declaring `name` as a parameter
std::optional<Declaration> findParameter(
    const std::vector<const ExprAST*>& chain, const std::string& name) {
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const PrototypeAST* proto = prototypeOf(**it);
    if (proto && declaresParameter(*proto, name)) {
      return Declaration{(*it)->getLocation(), "", *it, name};
    }
  }
  return std::nullopt;
}

// True when the offset lies in a definition's header, before its body
bool inHeader(const ExprAST& node, int offset, const std::string& source) {
  size_t brace = source.find('{', node.getLocation().offset);
  return brace == std::string::npos || offset < static_cast<int>(brace);
}

// The declaration for a cursor on a definition's own header: the definition,
// or the field, variant or binding written there
std::optional<Declaration> ownDeclaration(const ExprAST& node, int offset,
                                          const std::string& source) {
  switch (node.getType()) {
    case ASTNodeType::CLASS_DEFINITION: {
      for (const auto& field :
           static_cast<const ClassDefinitionAST&>(node).getFields()) {
        if (spanContains(field.location, offset)) {
          return Declaration{field.location, "", nullptr, field.name};
        }
      }
      break;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      for (const auto& field :
           static_cast<const InterfaceDefinitionAST&>(node).getFields()) {
        if (spanContains(field.location, offset)) {
          return Declaration{field.location, "", nullptr, field.name};
        }
      }
      break;
    }
    case ASTNodeType::ENUM_DEFINITION: {
      for (const auto& variant :
           static_cast<const EnumDefinitionAST&>(node).getVariants()) {
        if (spanContains(variant.location, offset)) {
          return Declaration{variant.location, "", nullptr, variant.name};
        }
      }
      break;
    }
    case ASTNodeType::MATCH: {
      for (const auto& arm : static_cast<const MatchExprAST&>(node).getArms()) {
        for (const auto& binding : arm.bindings) {
          if (!binding.isWildcard && spanContains(binding.location, offset)) {
            return Declaration{binding.location, "", nullptr, binding.name};
          }
        }
      }
      return std::nullopt;
    }
    case ASTNodeType::VARIABLE_CREATION:
    case ASTNodeType::REFERENCE_CREATION:
    case ASTNodeType::DECLARE_TYPE:
      return declarationOf(node);
    case ASTNodeType::FUNCTION:
    case ASTNodeType::FOR_IN_LOOP:
      break;
    default:
      return std::nullopt;
  }
  if (declarationName(node).empty() && node.getType() != ASTNodeType::FOR_IN_LOOP) {
    return std::nullopt;
  }
  if (!inHeader(node, offset, source)) return std::nullopt;
  if (node.getType() == ASTNodeType::FOR_IN_LOOP) {
    return Declaration{node.getLocation(), "", &node,
                       static_cast<const ForInExprAST&>(node).getLoopVar()};
  }
  return declarationOf(node);
}

// The declaration behind the node under the cursor
std::optional<Declaration> declarationUnder(const BlockExprAST& program,
                                            const Target& target, int offset,
                                            const std::string& source) {
  const ExprAST& node = target.node();
  if (auto own = ownDeclaration(node, offset, source)) return own;
  if (node.getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const std::string& name =
        static_cast<const VariableReferenceAST&>(node).getName();
    if (auto local = findLocalDeclaration(target.chain, node, name)) {
      return local;
    }
    if (auto parameter = findParameter(target.chain, name)) return parameter;
  }
  return findDeclarationOf(program, target.chain, node);
}

// Text of the file holding a declaration: the document, a file of the same
// manifest, or a library source read from disk
std::optional<std::string> textOf(const Position& location,
                                  const std::string& documentPath,
                                  const std::string& source) {
  std::string text = sourceFor(location, documentPath, source);
  if (!text.empty()) return text;
  std::ifstream file(*location.filePath);
  if (!file.is_open()) return std::nullopt;
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// The declared name's range, in the declaration's own file
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

}  // namespace

std::optional<DefinitionLocation> computeDefinition(
    const BlockExprAST& program, const std::string& filePath,
    const std::string& source, int byteOffset) {
  std::string documentPath = normalizePath(filePath);
  std::optional<Declaration> declaration;

  // A type name written in an annotation stands for its definition. This is
  // checked on the tree as parsed: specialization clones carry no annotation
  // spans, so the redirected lookup below cannot see them.
  NodeFinder finder(documentPath, byteOffset);
  finder.visit(program);
  if (finder.chain().empty()) return std::nullopt;
  if (const TypeAnnotation* annotation =
          annotationIn(*finder.chain().back(), byteOffset)) {
    if (const ExprAST* decl = findAnnotatedType(program, *annotation)) {
      declaration = declarationOf(*decl);
    }
  }

  if (!declaration) {
    std::optional<Target> target = locate(program, documentPath, byteOffset);
    if (!target) return std::nullopt;
    declaration = declarationUnder(program, *target, byteOffset, source);
  }
  // A declaration with no file cannot be opened (a bundle built before
  // source paths were recorded)
  if (!declaration || !declaration->location.filePath) return std::nullopt;

  std::optional<std::string> text =
      textOf(declaration->location, documentPath, source);
  if (!text) return std::nullopt;

  DefinitionLocation result;
  result.filePath = normalizePath(*declaration->location.filePath);
  result.range = nameRangeOf(*declaration, *text);
  result.start = lspPositionFromByteOffset(*text, result.range.offset);
  result.end = lspPositionFromByteOffset(
      *text, result.range.endOffset.value_or(result.range.offset));
  return result;
}

}  // namespace sun::lsp
