// module_ast.h — ModuleAST class

#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "semantic_analysis/qualified_name.h"

// Module declaration: module Name { declarations... }
// Also supports legacy 'namespace' keyword
class ModuleAST : public ExprAST {
  std::string name;
  std::unique_ptr<BlockExprAST> body;
  sun::QualifiedName qualifiedName;

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
  /** Return the defining module name, independent of source aliases. */
  const sun::QualifiedName& getQualifiedName() const { return qualifiedName; }
  /** Whether the module's defining name has been recorded. */
  bool hasQualifiedName() const { return !qualifiedName.baseName.empty(); }
  /** Record the module's defining name during import or semantic analysis. */
  void setQualifiedName(sun::QualifiedName value) {
    qualifiedName = std::move(value);
  }
  const BlockExprAST& getBody() const { return *body; }
  BlockExprAST& mutableBody() { return *body; }
  std::string dotLabel() const override { return "Module\n" + name; }
};
