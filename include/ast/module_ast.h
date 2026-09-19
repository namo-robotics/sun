// module_ast.h — ModuleAST class

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "semantic_analysis/qualified_name.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::semantic_analysis::QualifiedName;

/**
 * Module declaration: module Name { declarations... }
 * Also supports legacy 'namespace' keyword
 */
class ModuleAST : public ExprAST {
  std::string name;
  std::unique_ptr<BlockExprAST> body;
  QualifiedName qualifiedName;
  std::string doc;
  std::optional<sun::support::Position> nameLocation;

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  ModuleAST(std::string name, std::unique_ptr<BlockExprAST> body)
      : name(std::move(name)), body(std::move(body)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::MODULE; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (body) body->forEachChildSlot(fn);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return std::string(isPublic() ? "public " : "") + "module " + name + " " +
           body->toString();
  }

  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }
  /** Return the comment documenting this module. */
  const std::string& getDoc() const { return doc; }
  /** Store the comment documenting this module. */
  void setDoc(std::string value) { doc = std::move(value); }
  /** Return the source span of this module's name, when available. */
  const std::optional<sun::support::Position>& getNameLocation() const {
    return nameLocation;
  }
  /** Record the source span of this module's name. */
  void setNameLocation(sun::support::Position value) {
    nameLocation = std::move(value);
  }
  /** Return the defining module name, independent of source aliases. */
  const QualifiedName& getQualifiedName() const { return qualifiedName; }
  /** Whether the module's defining name has been recorded. */
  bool hasQualifiedName() const { return !qualifiedName.baseName.empty(); }
  /** Record the module's defining name during import or semantic analysis. */
  void setQualifiedName(QualifiedName value) {
    qualifiedName = std::move(value);
  }
  /** Return the next module in the same dotted declaration, or null. */
  const ModuleAST* getShorthandChild() const {
    const auto& statements = body->getBody();
    if (statements.size() != 1) return nullptr;
    const auto* child =
        dynamic_cast<const ModuleAST*>(statements.front().get());
    return child && child->getLocation().offset == getLocation().offset
               ? child
               : nullptr;
  }
  /** Return the next mutable module in the same dotted declaration, or null. */
  ModuleAST* getShorthandChild() {
    return const_cast<ModuleAST*>(std::as_const(*this).getShorthandChild());
  }
  /** Provides access to the expressions that make up the body. */
  const BlockExprAST& getBody() const { return *body; }
  /** Provides mutable access to the body so compiler passes can rewrite it. */
  BlockExprAST& mutableBody() { return *body; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Module\n" + name; }
};

}  // namespace sun::ast
