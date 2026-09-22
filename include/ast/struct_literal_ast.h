// struct_literal_ast.h — StructLiteralAST class

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Struct literal: { color: "red", speed: 120 }
 *
 * Constructs a class by naming each field, for classes that declare no `init`.
 * Positional construction is deliberately not offered for these: field order
 * is a layout detail, and a positional call would silently change meaning if
 * two same-typed fields were ever reordered.
 *
 * A literal carries no type of its own — it takes the type of the context it
 * appears in (`var car: Car = { ... }`), which semantic analysis supplies.
 */
class StructLiteralAST : public ExprAST {
 public:
  /** A named field and the expression supplying its initial value. */
  struct FieldInit {
    std::string name;
    std::unique_ptr<ExprAST> value;
    sun::support::Position location;  // the field name, for error reporting
  };

 private:
  std::vector<FieldInit> fields_;

 protected:
  /** Creates the analysis annotations required by this node when first needed.
   */
  void ensureAnalysis() const override {
    if (!analysis_) analysis_ = std::make_unique<StructLiteralAnalysis>();
  }

 public:
  /** Retain selected fields separately from their source names. */
  std::vector<sun::semantic_analysis::DeclarationId>& resolvedFields() const {
    return static_cast<StructLiteralAnalysis&>(analysis()).fields;
  }

  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  explicit StructLiteralAST(std::vector<FieldInit> fields)
      : fields_(std::move(fields)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::STRUCT_LITERAL; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& field : fields_) fn(field.value);
  }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = "{";
    for (size_t i = 0; i < fields_.size(); ++i) {
      if (i > 0) result += ",";
      result += " " + fields_[i].name + ": " + fields_[i].value->toString();
    }
    return result + " }";
  }

  /** Provides the field declarations belonging to this type. */
  const std::vector<FieldInit>& getFields() const { return fields_; }
  /** Provides mutable access to field declarations for later compiler passes.
   */
  std::vector<FieldInit>& getMutableFields() { return fields_; }
  /** Returns the number of stored entries. */
  size_t size() const { return fields_.size(); }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "StructLiteral"; }
};

}  // namespace sun::ast
