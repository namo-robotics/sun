// struct_literal_ast.h — StructLiteralAST class

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/expr_ast.h"

// Struct literal: { color: "red", speed: 120 }
//
// Constructs a class by naming each field, for classes that declare no `init`.
// Positional construction is deliberately not offered for these: field order
// is a layout detail, and a positional call would silently change meaning if
// two same-typed fields were ever reordered.
//
// A literal carries no type of its own — it takes the type of the context it
// appears in (`var car: Car = { ... }`), which semantic analysis supplies.
class StructLiteralAST : public ExprAST {
 public:
  struct FieldInit {
    std::string name;
    std::unique_ptr<ExprAST> value;
    Position location;  // the field name, for error reporting
  };

 private:
  std::vector<FieldInit> fields_;

 public:
  explicit StructLiteralAST(std::vector<FieldInit> fields)
      : fields_(std::move(fields)) {}

  ASTNodeType getType() const override { return ASTNodeType::STRUCT_LITERAL; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& field : fields_) fn(field.value);
  }

  std::string toString() const override {
    std::string result = "{";
    for (size_t i = 0; i < fields_.size(); ++i) {
      if (i > 0) result += ",";
      result += " " + fields_[i].name + ": " + fields_[i].value->toString();
    }
    return result + " }";
  }

  const std::vector<FieldInit>& getFields() const { return fields_; }
  std::vector<FieldInit>& getMutableFields() { return fields_; }
  size_t size() const { return fields_.size(); }

  std::string dotLabel() const override { return "StructLiteral"; }
};
