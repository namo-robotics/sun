// module_ast.h — ModuleAST class

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "semantic_analysis/qualified_name.h"

// Module declaration: module Name { declarations... }
// Also supports legacy 'namespace' keyword
class ModuleAST : public ExprAST {
  std::string name;
  std::unique_ptr<BlockExprAST> body;
  sun::QualifiedName qualifiedName;
  std::string doc;
  std::optional<Position> nameLocation;

 public:
  ModuleAST(std::string name, std::unique_ptr<BlockExprAST> body)
      : name(std::move(name)), body(std::move(body)) {}

  ASTNodeType getType() const override { return ASTNodeType::MODULE; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (body) body->forEachChildSlot(fn);
  }
  std::string toString() const override {
    return std::string(isPublic() ? "public " : "") + "module " + name + " " +
           body->toString();
  }

  const std::string& getName() const { return name; }
  /** Return the comment documenting this module. */
  const std::string& getDoc() const { return doc; }
  /** Store the comment documenting this module. */
  void setDoc(std::string value) { doc = std::move(value); }
  /** Return the source span of this module's name, when available. */
  const std::optional<Position>& getNameLocation() const { return nameLocation; }
  /** Record the source span of this module's name. */
  void setNameLocation(Position value) { nameLocation = std::move(value); }
  /** Return the defining module name, independent of source aliases. */
  const sun::QualifiedName& getQualifiedName() const { return qualifiedName; }
  /** Whether the module's defining name has been recorded. */
  bool hasQualifiedName() const { return !qualifiedName.baseName.empty(); }
  /** Record the module's defining name during import or semantic analysis. */
  void setQualifiedName(sun::QualifiedName value) {
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
  const BlockExprAST& getBody() const { return *body; }
  BlockExprAST& mutableBody() { return *body; }
  std::string dotLabel() const override { return "Module\n" + name; }
};
