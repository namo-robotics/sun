// rename.h — Rename symbol for the language server

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "lsp/symbol_location.h"

namespace sun::lsp {

struct Rename {
  std::string name;  // The name as currently written
  std::vector<SymbolLocation>
      sites;            // Every place to edit, declaration included
  std::string refusal;  // Why the symbol cannot be renamed, or empty
};

// The symbol at byteOffset with every place its name is written, found as
// computeReferences finds them. An interface member and the class members
// implementing it are renamed together, since renaming one side alone would
// break the program. A symbol declared in a library (loaded from a .moon
// bundle) is refused, since its uses inside the library cannot be edited.
// Nothing when the cursor is not on a symbol.
std::optional<Rename> computeRename(const BlockExprAST& program,
                                    const std::string& filePath,
                                    const std::string& source, int byteOffset);

// The site in `filePath` holding byteOffset, cursor at the end included;
// nothing when the cursor is outside every site
std::optional<SymbolLocation> siteAt(const Rename& rename,
                                     const std::string& filePath,
                                     int byteOffset);

// Why newName cannot be a symbol name, or empty when it can: it must lex as
// a single identifier, so keywords and punctuation are refused
std::string checkNewName(const std::string& newName);

}  // namespace sun::lsp
