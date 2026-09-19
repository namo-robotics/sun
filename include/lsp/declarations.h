// declarations.h — Finding the node under the cursor and the declaration a
// symbol names. Shared by the language server's hover and go-to-definition.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/block_expr_ast.h"
#include "ast/try_catch_expr_ast.h"
#include "ast/type_annotation.h"
#include "semantic_analysis/qualified_name.h"
#include "support/position.h"
#include "types/types.h"

/** Provides compiler-backed editor features through the language server protocol. */
namespace sun::lsp {
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::semantic_analysis::QualifiedName;

/**
 * Type parameter name -> the type it stands for in one specialization
 */
using Bindings = std::vector<std::pair<std::string, sun::types::TypePtr>>;

/**
 * Canonical form of a path that exists on disk; other paths are unchanged
 */
std::string normalizePath(const std::string& path);

/** Reports whether a source span contains the requested byte offset. */
bool spanContains(const sun::support::Position& loc, int offset);

/**
 * Text covered by a span, or empty when the span is missing or out of range
 */
std::string sliceSpan(const std::string& source,
                      const sun::support::Position& loc);

// ---------------------------------------------------------------------------
// Locating the node under the cursor
// ---------------------------------------------------------------------------

/**
 * Descends to the innermost node containing the offset, keeping its ancestors
 */
class NodeFinder {
 public:
  /** Selects the document and byte offset to search in a syntax tree. */
  NodeFinder(std::string documentPath, int offset)
      : documentPath_(std::move(documentPath)), offset_(offset) {}

  /**
   * Ancestors of the innermost node, outermost first
   */
  const std::vector<const ExprAST*>& chain() const { return chain_; }

  /**
   * True when the subtree rooted at node covers the offset
   */
  bool visit(const ExprAST& node);

 private:
  /** Reports whether a source position belongs to the requested editor document. */
  bool isDocumentFile(const sun::support::Position& loc);

  std::string documentPath_;
  int offset_;
  std::unordered_map<std::string, bool> fileMatches_;
  std::vector<const ExprAST*> chain_;
};

/**
 * The node under the cursor with its ancestors (outermost first) and the type
 * parameter bindings in effect when it sits inside a generic body
 */
struct Target {
  std::vector<const ExprAST*> chain;
  Bindings bindings;
  /** Returns the syntax node selected at the requested source offset. */
  const ExprAST& node() const { return *chain.back(); }
};

/**
 * Node chain at the offset. Generic templates are analyzed only through their
 * specializations, so the chain is redirected into the first specialization
 * of any generic class or function it passes through; the clones keep the
 * template's source spans.
 */
std::optional<Target> locate(const BlockExprAST& program,
                             const std::string& documentPath, int offset);

/**
 * First specialization of a generic class or function, with its bindings;
 * null when the node is not a generic template or was never used
 */
const ExprAST* firstSpecialization(const ExprAST& node, Bindings& bindings);

// ---------------------------------------------------------------------------
// Finding the declaration behind a symbol
// ---------------------------------------------------------------------------

/**
 * A declaration found for a symbol: where it is, and the comment stored on
 * it when the tree carries one (declarations loaded from a bundle). When the
 * stored comment is empty, the source at the location is consulted.
 */
struct Declaration {
  sun::support::Position location;
  std::string doc;
  const ExprAST* node = nullptr;  // Declaring node, when there is one
  // Declared name; empty when `location` already is the name token (a field,
  // a variant, a match binding)
  std::string name;
};

/** Reports whether the syntax-node kind introduces a declaration. */
bool isDefinition(sun::ast::ASTNodeType kind);

/**
 * Name a module-level declaration is known by, or empty for other nodes
 */
std::string declarationName(const ExprAST& node);

/**
 * Qualified name the analyzer gave a declaration, or empty
 */
QualifiedName declarationQualifiedName(const ExprAST& node);

/** Extracts declaration information from a syntax node. */
Declaration declarationOf(const ExprAST& node);

/**
 * Module-level declaration with this name; a qualified-name match wins over
 * a plain name match when the reference was resolved by the analyzer
 */
const ExprAST* findDeclaration(const BlockExprAST& program,
                               const std::string& name,
                               const QualifiedName& qualified);

/** Unwraps reference types to inspect the underlying value type. */
const sun::types::Type* stripReference(const sun::types::Type* type);

/**
 * Definition node (class, interface or enum) behind a type
 */
const ExprAST* findTypeDefinition(const BlockExprAST& program,
                                  const sun::types::Type& type);

/**
 * A member (method, field or variant) inside a definition
 */
std::optional<Declaration> findMember(const ExprAST& definition,
                                      const std::string& member);

/**
 * Declaration of a local name visible at `node`: the closest earlier
 * `var`/`const`/`ref` in an enclosing block, or an enclosing loop variable.
 * Parameters have no declaration of their own and yield nothing.
 */
std::optional<Declaration> findLocalDeclaration(
    const std::vector<const ExprAST*>& chain, const ExprAST& node,
    const std::string& name);

/**
 * Where the symbol under `node` was declared, or nothing
 */
std::optional<Declaration> findDeclarationOf(
    const BlockExprAST& program, const std::vector<const ExprAST*>& chain,
    const ExprAST& node);

/**
 * The member behind `object.member`: a module's item (the analyzer records
 * which module's in `qualifiedName`), or a field, method or variant
 * of the object's type. A match pattern's object carries no type, so a bare
 * name there is looked up as a type.
 */
std::optional<Declaration> findMemberDeclaration(
    const BlockExprAST& program, const ExprAST& object,
    const std::string& member, const QualifiedName& qualifiedName);

/**
 * The nearest enclosing function or lambda declaring `name` as a parameter
 */
std::optional<Declaration> findParameter(
    const std::vector<const ExprAST*>& chain, const std::string& name);

/**
 * The declaration a name-bearing node refers to: a local, a parameter, or
 * what findDeclarationOf finds. Used for the cursor and for every candidate
 * reference alike, so both land on the same declaration.
 */
std::optional<Declaration> resolveSymbol(
    const BlockExprAST& program, const std::vector<const ExprAST*>& chain,
    const ExprAST& node);

/**
 * The declaration for a cursor on a definition's own header: the definition,
 * or the field, variant, parameter or binding written there
 */
std::optional<Declaration> ownDeclaration(const ExprAST& node, int offset,
                                          const std::string& source);

/**
 * The declaration behind the node under the cursor
 */
std::optional<Declaration> declarationUnder(const BlockExprAST& program,
                                            const Target& target, int offset,
                                            const std::string& source);

/**
 * Where the symbol at byteOffset was declared: the type a written annotation
 * names, else the declaration behind the node under the cursor
 */
std::optional<Declaration> findDeclarationAt(const BlockExprAST& program,
                                             const std::string& documentPath,
                                             const std::string& source,
                                             int byteOffset);

/**
 * Catch clauses with the span that declares each one's binding: a binding
 * has no position of its own, so it is the text between the previous block
 * and the clause's body
 */
using CatchBindingFn = std::function<void(
    const sun::ast::CatchClause&, const sun::support::Position& header)>;
/** Visits the error bindings introduced by catch clauses. */
void forEachCatchBinding(const sun::ast::TryCatchExprAST& tryCatch,
                         const CatchBindingFn& fn);

/**
 * Text of the file a declaration lives in: the document itself, or a file
 * registered during compilation (another file of the same manifest); empty
 * when neither
 */
std::string sourceFor(const sun::support::Position& declaration,
                      const std::string& documentPath,
                      const std::string& documentSource);

/**
 * Every annotation written on a node: parameter and return types, a
 * variable's or loop variable's type, field and payload types, type
 * arguments, catch binding types
 */
using AnnotationFn = std::function<void(const sun::ast::TypeAnnotation&)>;
/** Visits type annotations directly associated with a syntax node. */
void forEachAnnotation(const ExprAST& node, const AnnotationFn& fn);

/**
 * The annotation under the cursor among those written on a node, or null
 */
const sun::ast::TypeAnnotation* annotationIn(const ExprAST& node, int offset);

/**
 * The user-defined type an annotation names, looking through `ref`,
 * pointer and array wrappers when their element has no span of its own
 */
const ExprAST* findAnnotatedType(const BlockExprAST& program,
                                 const sun::ast::TypeAnnotation& annotation);

}  // namespace sun::lsp
