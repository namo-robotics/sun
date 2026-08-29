// rename.cpp — Rename symbol for the language server
//
// The symbol under the cursor is resolved to its declaration, widened to
// its member group (an interface member with its implementations), then
// every place any of them is named becomes an edit site.

#include "lsp/rename.h"

#include <cctype>
#include <sstream>

#include "ast.h"
#include "lsp/declarations.h"
#include "lsp/references.h"
#include "parsing/lexer.h"

namespace sun::lsp {

namespace {

bool isIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

}  // namespace

std::optional<Rename> computeRename(const BlockExprAST& program,
                                    const std::string& filePath,
                                    const std::string& source, int byteOffset) {
  std::string documentPath = normalizePath(filePath);
  std::optional<Declaration> declaration =
      findDeclarationAt(program, documentPath, source, byteOffset);
  // A cursor just after a name still means that name
  if (!declaration && byteOffset > 0 &&
      static_cast<size_t>(byteOffset) <= source.size() &&
      isIdentifierChar(source[byteOffset - 1]) &&
      (static_cast<size_t>(byteOffset) == source.size() ||
       !isIdentifierChar(source[byteOffset]))) {
    declaration =
        findDeclarationAt(program, documentPath, source, byteOffset - 1);
  }
  if (!declaration) return std::nullopt;

  MemberGroup group = memberGroupOf(program, *declaration, documentPath);
  Occurrences occurrences =
      findOccurrences(program, documentPath, source, group.members, true);

  Rename rename;
  rename.sites = std::move(occurrences.locations);
  // The cursor must sit on a name that will be edited: `this` resolves to
  // its class without naming it, and a declaration whose name was not found
  // cannot be renamed safely
  std::optional<SymbolLocation> site = siteAt(rename, documentPath, byteOffset);
  if (!site) return std::nullopt;
  rename.name = source.substr(
      site->range.offset,
      site->range.endOffset.value_or(site->range.offset) - site->range.offset);
  if (!occurrences.allDeclared) {
    rename.refusal =
        "'" + rename.name + "' is declared in a library and cannot be renamed";
  } else if (!group.builtinInterface.empty()) {
    rename.refusal = "'" + rename.name +
                     "' implements a member of the builtin " +
                     group.builtinInterface + " and cannot be renamed";
  }
  return rename;
}

std::optional<SymbolLocation> siteAt(const Rename& rename,
                                     const std::string& filePath,
                                     int byteOffset) {
  std::string documentPath = normalizePath(filePath);
  for (const SymbolLocation& site : rename.sites) {
    if (site.filePath != documentPath) continue;
    int end = site.range.endOffset.value_or(site.range.offset);
    if (site.range.offset <= byteOffset && byteOffset <= end) return site;
  }
  return std::nullopt;
}

std::string checkNewName(const std::string& newName) {
  if (newName.empty()) return "A name is required";
  // A name the lexer rejects outright (say 1abc, an invalid literal suffix)
  // is just as invalid as one that lexes to something other than an
  // identifier.
  try {
    std::istringstream stream(newName);
    Lexer lexer(stream);
    Token first = lexer.getNextToken();
    if (isKeyword(first.kind)) return "'" + newName + "' is a keyword";
    if (first.kind != TokenKind::IDENTIFIER || first.text != newName ||
        !lexer.getNextToken().isEof()) {
      return "'" + newName + "' is not a valid name";
    }
  } catch (const std::exception&) {
    return "'" + newName + "' is not a valid name";
  }
  return "";
}

}  // namespace sun::lsp
