// declarations.h — Finding the node under the cursor and the declaration a
// symbol names. Shared by the language server's hover and go-to-definition.

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/block_expr_ast.h"
#include "ast/type_annotation.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/types.h"
#include "support/position.h"

namespace sun::lsp {

// Type parameter name -> the type it stands for in one specialization
using Bindings = std::vector<std::pair<std::string, sun::TypePtr>>;

// Canonical form of a path that exists on disk; other paths are unchanged
std::string normalizePath(const std::string& path);

bool spanContains(const Position& loc, int offset);

// Text covered by a span, or empty when the span is missing or out of range
std::string sliceSpan(const std::string& source, const Position& loc);

// ---------------------------------------------------------------------------
// Locating the node under the cursor
// ---------------------------------------------------------------------------

// Descends to the innermost node containing the offset, keeping its ancestors
class NodeFinder {
 public:
  NodeFinder(std::string documentPath, int offset)
      : documentPath_(std::move(documentPath)), offset_(offset) {}

  // Ancestors of the innermost node, outermost first
  const std::vector<const ExprAST*>& chain() const { return chain_; }

  // True when the subtree rooted at node covers the offset
  bool visit(const ExprAST& node);

 private:
  bool isDocumentFile(const Position& loc);

  std::string documentPath_;
  int offset_;
  std::unordered_map<std::string, bool> fileMatches_;
  std::vector<const ExprAST*> chain_;
};

// The node under the cursor with its ancestors (outermost first) and the type
// parameter bindings in effect when it sits inside a generic body
struct Target {
  std::vector<const ExprAST*> chain;
  Bindings bindings;
  const ExprAST& node() const { return *chain.back(); }
};

// Node chain at the offset. Generic templates are analyzed only through their
// specializations, so the chain is redirected into the first specialization
// of any generic class or function it passes through; the clones keep the
// template's source spans.
std::optional<Target> locate(const BlockExprAST& program,
                             const std::string& documentPath, int offset);

// ---------------------------------------------------------------------------
// Finding the declaration behind a symbol
// ---------------------------------------------------------------------------

// A declaration found for a symbol: where it is, and the comment stored on
// it when the tree carries one (declarations loaded from a bundle). When the
// stored comment is empty, the source at the location is consulted.
struct Declaration {
  Position location;
  std::string doc;
  const ExprAST* node = nullptr;  // Declaring node, when there is one
  // Declared name; empty when `location` already is the name token (a field,
  // a variant, a match binding)
  std::string name;
};

bool isDefinition(ASTNodeType kind);

// Name a module-level declaration is known by, or empty for other nodes
std::string declarationName(const ExprAST& node);

// Qualified name the analyzer gave a declaration, or empty
sun::QualifiedName declarationQualifiedName(const ExprAST& node);

Declaration declarationOf(const ExprAST& node);

// Module-level declaration with this name; a qualified-name match wins over
// a plain name match when the reference was resolved by the analyzer
const ExprAST* findDeclaration(const BlockExprAST& program,
                               const std::string& name,
                               const sun::QualifiedName& qualified);

const sun::Type* stripReference(const sun::Type* type);

// Definition node (class, interface or enum) behind a type
const ExprAST* findTypeDefinition(const BlockExprAST& program,
                                  const sun::Type& type);

// A member (method, field or variant) inside a definition
std::optional<Declaration> findMember(const ExprAST& definition,
                                      const std::string& member);

// Declaration of a local name visible at `node`: the closest earlier
// `var`/`const`/`ref` in an enclosing block, or an enclosing loop variable.
// Parameters have no declaration of their own and yield nothing.
std::optional<Declaration> findLocalDeclaration(
    const std::vector<const ExprAST*>& chain, const ExprAST& node,
    const std::string& name);

// Where the symbol under `node` was declared, or nothing
std::optional<Declaration> findDeclarationOf(
    const BlockExprAST& program, const std::vector<const ExprAST*>& chain,
    const ExprAST& node);

// Text of the file a declaration lives in: the document itself, or a file
// registered during compilation (another file of the same manifest); empty
// when neither
std::string sourceFor(const Position& declaration,
                      const std::string& documentPath,
                      const std::string& documentSource);

// The annotation under the cursor among those written on a node, or null
const TypeAnnotation* annotationIn(const ExprAST& node, int offset);

// The user-defined type an annotation names, looking through `ref`,
// pointer and array wrappers when their element has no span of its own
const ExprAST* findAnnotatedType(const BlockExprAST& program,
                                 const TypeAnnotation& annotation);

}  // namespace sun::lsp
