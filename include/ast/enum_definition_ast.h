// enum_definition_ast.h — EnumDefinitionAST class

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"
#include "lexer.h"

// Enum variant declaration: Red, Green, Blue (for now, without associated data)
struct EnumVariantDecl {
  std::string name;
  int64_t value;      // Explicit or implicit numeric value
  Position location;  // Source location of variant declaration
};

// Enum definition: enum Name { Variant1, Variant2, ... }
// For now, simple C-style enums without associated data
class EnumDefinitionAST : public ExprAST {
  std::string name;
  std::vector<EnumVariantDecl> variants;

 public:
  EnumDefinitionAST(std::string name, std::vector<EnumVariantDecl> variants,
                    bool precompiled = false)
      : name(std::move(name)), variants(std::move(variants)) {
    precompiled_ = precompiled;
  }

  ASTNodeType getType() const override { return ASTNodeType::ENUM_DEFINITION; }
  std::string toString() const override {
    std::string result = "enum " + name + " { ";
    for (size_t i = 0; i < variants.size(); ++i) {
      if (i > 0) result += ", ";
      result += variants[i].name;
    }
    result += " }";
    return result;
  }

  const std::string& getName() const { return name; }
  const std::vector<EnumVariantDecl>& getVariants() const { return variants; }

  // Get a variant by name
  const EnumVariantDecl* getVariant(const std::string& variantName) const {
    for (const auto& variant : variants) {
      if (variant.name == variantName) return &variant;
    }
    return nullptr;
  }

  // Get the number of variants
  size_t getNumVariants() const { return variants.size(); }
  std::string dotLabel() const override { return "Enum\n" + name; }
};
