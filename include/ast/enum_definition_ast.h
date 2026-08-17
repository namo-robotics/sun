// enum_definition_ast.h — EnumDefinitionAST class

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <map>

#include "ast/expr_ast.h"
#include "ast/type_annotation.h"
#include "lexer.h"

namespace sun {
class EnumType;
}

// Enum variant declaration: Red, Circle(f64), Rect(f64, f64)
struct EnumVariantDecl {
  std::string name;
  int64_t value;      // Explicit or implicit numeric value
  Position location;  // Source location of variant declaration
  std::vector<TypeAnnotation> payloadTypes;  // empty = unit variant

  bool hasPayload() const { return !payloadTypes.empty(); }
};

// Enum definition: enum Name { Variant1, Variant2(T1, T2), ... }
// Generic form: enum Option<T> { Some(T), None }
class EnumDefinitionAST : public ExprAST {
  std::string name;
  std::vector<EnumVariantDecl> variants;
  std::vector<std::string> typeParameters;  // empty = non-generic
  // Populated during semantic analysis (mutable, like ClassAnalysis
  // specializations on ClassDefinitionAST)
  mutable std::map<std::string, std::shared_ptr<sun::EnumType>>
      specializations_;

 public:
  EnumDefinitionAST(std::string name, std::vector<EnumVariantDecl> variants,
                    bool precompiled = false,
                    std::vector<std::string> typeParams = {})
      : name(std::move(name)),
        variants(std::move(variants)),
        typeParameters(std::move(typeParams)) {
    precompiled_ = precompiled;
  }

  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  bool isGeneric() const { return !typeParameters.empty(); }

  // Specialization storage for generic enums, mirroring generic classes.
  // Enums carry no per-specialization code, so the artifact is the resolved
  // EnumType itself (payload types substituted). Called by the semantic
  // analyzer when the generic enum is instantiated; codegen walks these to
  // build the storage structs.
  void addSpecialization(const std::string& mangledName,
                         std::shared_ptr<sun::EnumType> specialized) const {
    specializations_[mangledName] = std::move(specialized);
  }
  const std::map<std::string, std::shared_ptr<sun::EnumType>>&
  getSpecializations() const {
    return specializations_;
  }

  ASTNodeType getType() const override { return ASTNodeType::ENUM_DEFINITION; }
  std::string toString() const override {
    std::string result = std::string(isPublic() ? "public " : "") + "enum " + name + " { ";
    for (size_t i = 0; i < variants.size(); ++i) {
      if (i > 0) result += ", ";
      result += variants[i].name;
      if (variants[i].hasPayload()) {
        result += "(";
        for (size_t j = 0; j < variants[i].payloadTypes.size(); ++j) {
          if (j > 0) result += ", ";
          result += variants[i].payloadTypes[j].baseName;
        }
        result += ")";
      }
    }
    result += " }";
    return result;
  }

  // True if any variant carries a payload (tagged-union representation)
  bool hasAnyPayload() const {
    for (const auto& v : variants) {
      if (v.hasPayload()) return true;
    }
    return false;
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
