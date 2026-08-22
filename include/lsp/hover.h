// hover.h — Type information for the language server's hover request

#pragma once

#include <optional>
#include <string>

#include "ast/block_expr_ast.h"
#include "position.h"

namespace sun::lsp {

struct Hover {
  std::string code;           // Sun-syntax text shown to the user
  std::string documentation;  // Comment written above the symbol's declaration
  Position range;             // Span of the hovered construct (byte offsets)
};

// Innermost node from `filePath` whose span contains byteOffset, or null.
// Nodes without a span (module wrappers from merged compilation) are looked
// through; moon import stubs and nodes from other files are skipped.
const ExprAST* findInnermostNodeAt(const BlockExprAST& program,
                                   const std::string& filePath,
                                   int byteOffset);

// Hover text for the construct at byteOffset in an analyzed program:
// `name: T` for variables and members, full signatures for functions,
// `class Name` for definitions, the type alone for literals and operators.
// Inside a generic class or function body, types come from the first
// specialization and are printed in terms of the type parameters.
// `source` is the document text; declarations whose types were never
// resolved (analysis stopped at an earlier error, or a generic that is never
// used) fall back to the type annotation written there. The comment directly
// above the symbol's declaration is returned as documentation. Returns
// nothing for statements and untyped nodes.
std::optional<Hover> computeHover(const BlockExprAST& program,
                                  const std::string& filePath,
                                  const std::string& source, int byteOffset);

}  // namespace sun::lsp
