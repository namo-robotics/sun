// references.h — Find references for the language server

#pragma once

#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "lsp/declarations.h"
#include "lsp/symbol_location.h"

namespace sun::lsp {

// Identity of a declaration: where it is. Specialization clones keep the
// template's spans, so a declaration reached through a clone and through
// the template compare equal. Parameters share their function's span and
// are told apart by name.
struct DeclarationKey {
  std::string file;
  int offset = 0;
  int end = -1;
  std::string parameter;
  bool operator==(const DeclarationKey&) const = default;
};

// A location without a file belongs to `file`
DeclarationKey declarationKey(const Declaration& declaration,
                              const std::string& file);

// A member and the members it shares a name with across an `implements`
// relation: an interface member with the member of every implementing
// class, or a class member with the interface member it implements and the
// other implementers. They are one symbol to references and rename, since
// changing one side alone would break the program.
struct MemberGroup {
  std::vector<Declaration> members;  // The declaration itself when no group
  // Name of a builtin interface (declared by the compiler, with no node in
  // any tree) whose member the declaration implements; the group cannot
  // list it, so a rename must be refused
  std::string builtinInterface;
};

MemberGroup memberGroupOf(const BlockExprAST& program,
                          const Declaration& declaration,
                          const std::string& documentPath);

struct Occurrences {
  std::vector<SymbolLocation> locations;  // Sorted by file then offset
  // Every target's declaration sits in the walked tree; false when one was
  // loaded from a .moon bundle
  bool allDeclared = false;
};

// Where each of `targets` is named, as computeReferences lists them, merged
// into one sorted list. Declarations outside the tree are not added.
Occurrences findOccurrences(const BlockExprAST& program,
                            const std::string& documentPath,
                            const std::string& source,
                            const std::vector<Declaration>& targets,
                            bool includeDeclaration);

// Every place the symbol at byteOffset is named, across all files of the
// analyzed program, sorted by file then offset: uses of a local, parameter,
// loop variable, match or catch binding, function, class, interface, enum,
// variant, field or method, including type names written in annotations
// and `implements` lists. With includeDeclaration the declared name is
// listed too; a declaration from a .moon bundle resolves to the library's
// source file when the bundle recorded it. Uses inside library code are not
// listed, since bundles carry no function bodies. An interface member and
// the class members implementing it are listed together, as one group.
// Empty for literals, operators, statements and whitespace.
std::vector<SymbolLocation> computeReferences(const BlockExprAST& program,
                                              const std::string& filePath,
                                              const std::string& source,
                                              int byteOffset,
                                              bool includeDeclaration);

}  // namespace sun::lsp
