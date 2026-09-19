// definition.cpp — Go-to-definition for the language server

#include "lsp/definition.h"

#include "lsp/declarations.h"
#include "lsp/name_ranges.h"

/** Provides compiler-backed editor features through the language server protocol. */
namespace sun::lsp {

/** Returns the definition location for the symbol at a document offset. */
std::optional<SymbolLocation> computeDefinition(
    const sun::ast::BlockExprAST& program, const std::string& filePath,
    const std::string& source, int byteOffset) {
  std::string documentPath = normalizePath(filePath);
  std::optional<Declaration> declaration =
      findDeclarationAt(program, documentPath, source, byteOffset);
  // A declaration with no file cannot be opened (a bundle built before
  // source paths were recorded)
  if (!declaration || !declaration->location.filePath) return std::nullopt;

  std::optional<std::string> text =
      textOf(declaration->location, documentPath, source);
  if (!text) return std::nullopt;
  return makeSymbolLocation(normalizePath(*declaration->location.filePath),
                            nameRangeOf(*declaration, *text), *text);
}

}  // namespace sun::lsp
