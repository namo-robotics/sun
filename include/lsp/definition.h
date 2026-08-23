// definition.h — Go-to-definition for the language server

#pragma once

#include <optional>
#include <string>

#include "ast/block_expr_ast.h"
#include "lsp/text_positions.h"
#include "support/position.h"

namespace sun::lsp {

struct DefinitionLocation {
  std::string filePath;  // Absolute path of the file holding the declaration
  Position range;        // Byte offsets of the declared name in that file
  LspPosition start;     // The same range in protocol coordinates
  LspPosition end;
};

// Where the symbol at byteOffset was declared: a local, a parameter, a loop
// variable, a match or catch binding, a module-level function, class,
// interface, enum, variable or alias, a member behind `obj.member`, the
// class behind `this`, or the type a written annotation names. On a
// declaration's own header the declaration itself is returned. The range
// covers the declared name. Declarations from a .moon bundle resolve to the
// library's source file when the bundle recorded it and the file can be
// read. Returns nothing for literals, operators, statements and whitespace.
std::optional<DefinitionLocation> computeDefinition(
    const BlockExprAST& program, const std::string& filePath,
    const std::string& source, int byteOffset);

}  // namespace sun::lsp
