// references.h — Find references for the language server

#pragma once

#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "lsp/symbol_location.h"

namespace sun::lsp {

// Every place the symbol at byteOffset is named, across all files of the
// analyzed program, sorted by file then offset: uses of a local, parameter,
// loop variable, match or catch binding, function, class, interface, enum,
// variant, field or method, including type names written in annotations
// and `implements` lists. With includeDeclaration the declared name is
// listed too; a declaration from a .moon bundle resolves to the library's
// source file when the bundle recorded it. Uses inside library code are not
// listed, since bundles carry no function bodies. An interface method and
// the class method implementing it are separate symbols. Empty for
// literals, operators, statements and whitespace.
std::vector<SymbolLocation> computeReferences(const BlockExprAST& program,
                                              const std::string& filePath,
                                              const std::string& source,
                                              int byteOffset,
                                              bool includeDeclaration);

}  // namespace sun::lsp
