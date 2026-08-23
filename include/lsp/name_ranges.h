// name_ranges.h — Narrowing a declaration's span to the name written in it.
// Most spans cover a whole declaration, so the name is found in the text.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ast/expr_ast.h"
#include "ast/prototype_ast.h"
#include "lsp/declarations.h"
#include "lsp/symbol_location.h"
#include "support/position.h"

namespace sun::lsp {

// Offset of the first whole word `name` in text[from, to), or -1
int findWord(const std::string& text, const std::string& name, size_t from,
             size_t to);

// `length` bytes at `offset` in the file of `base`
Position rangeAt(const Position& base, int offset, int length);

// True when `word` is written at `offset`
bool textHas(const std::string& text, int offset, const std::string& word);

// The name token of a declaration: the span itself when it already is the
// name, otherwise the first whole word `name` inside it, or the name just
// before it (a signature span starts at its parenthesis). Falls back to an
// empty range at the start of the span.
Position nameRange(const Position& span, const std::string& name,
                   const std::string& text);

// Signature of a function or lambda; null for other nodes
const PrototypeAST* prototypeOf(const ExprAST& node);

bool declaresParameter(const PrototypeAST& proto, const std::string& name);

// Where `name` is declared in a signature: `name:` or `name...` after the
// opening parenthesis
std::optional<Position> parameterRange(const ExprAST& owner,
                                       const std::string& name,
                                       const std::string& text);

// Text of the file holding a location: the document, a file of the same
// manifest, or a library source read from disk
std::optional<std::string> textOf(const Position& location,
                                  const std::string& documentPath,
                                  const std::string& source);

// The declared name's range, in the declaration's own file
Position nameRangeOf(const Declaration& declaration, const std::string& text);

}  // namespace sun::lsp
