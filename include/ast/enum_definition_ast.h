// enum_definition_ast.h — EnumDefinitionAST class

#pragma once

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::types {
class EnumType;
}

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"
#include "ast/type_annotation.h"
#include "parsing/lexer.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::semantic_analysis::DeclarationId;
using sun::semantic_analysis::QualifiedName;

}  // namespace sun::ast

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Enum variant declaration: Red, Circle(f64), Rect(f64, f64)
 */
struct EnumVariantDecl {
  std::string name;
  int64_t value;  // Tag bits; the enum type determines signedness
  sun::support::Position location;  // Source location of variant declaration
  std::vector<TypeAnnotation> payloadTypes;  // empty = unit variant
  std::string doc;  // Comment written above the variant

  bool hasExplicitValue = false;

  /** Reports whether this object has payload. */
  bool hasPayload() const { return !payloadTypes.empty(); }
  mutable sun::semantic_analysis::DeclarationIdentity declaration{};
};

/**
 * Enum definition: enum Name { Variant1, Variant2(T1, T2), ... }
 * Generic form: enum Option&lt;T&gt; { Some(T), None }
 */
class EnumDefinitionAST : public ExprAST {
  std::string name;
  std::string underlyingType;  // Empty means the default representation
  std::vector<EnumVariantDecl> variants;
  std::vector<TypeParameter> typeParameters;  // empty = non-generic
  std::string doc_;                           // Comment written above the enum
  // Populated during semantic analysis (mutable, like ClassAnalysis
  // specializations on ClassDefinitionAST)
  mutable std::map<DeclarationId, std::shared_ptr<sun::types::EnumType>>
      specializations_;

 public:
  QualifiedName qualifiedName;
  /** The original declaration name, retained by imported enums. */
  const QualifiedName& getQualifiedName() const { return qualifiedName; }
  /** Whether this enum already carries its defining name. */
  bool hasQualifiedName() const { return !qualifiedName.baseName.empty(); }
  /** Record the defining name independently of visible aliases. */
  void setQualifiedName(QualifiedName name) { qualifiedName = std::move(name); }
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  EnumDefinitionAST(std::string name, std::vector<EnumVariantDecl> variants,
                    bool precompiled = false,
                    std::vector<TypeParameter> typeParams = {},
                    std::string underlyingType = {})
      : name(std::move(name)),
        underlyingType(std::move(underlyingType)),
        variants(std::move(variants)),
        typeParameters(std::move(typeParams)) {
    precompiled_ = precompiled;
  }

  /** Return the written integer type, or an empty string for the default. */
  const std::string& getUnderlyingType() const { return underlyingType; }
  /** Return the integer representation used for this enum. */
  std::string getUnderlyingTypeName() const {
    return underlyingType.empty() ? "i32" : underlyingType;
  }
  /** Format a tag according to the enum's signedness. */
  std::string getValueText(const EnumVariantDecl& variant) const {
    return !underlyingType.empty() && underlyingType[0] == 'u'
               ? std::to_string(static_cast<uint64_t>(variant.value))
               : std::to_string(variant.value);
  }

  /** Provides the generic parameters declared by this type or function. */
  const std::vector<TypeParameter>& getTypeParameters() const {
    return typeParameters;
  }
  /** Returns the names used to bind generic arguments during specialization. */
  std::vector<std::string> getTypeParameterNames() const {
    return typeParameterNames(typeParameters);
  }
  /** Reports whether this declaration still has unbound type parameters. */
  bool isGeneric() const { return !typeParameters.empty(); }

  /**
   * Specialization storage for generic enums, mirroring generic classes.
   * Enums carry no per-specialization code, so the artifact is the resolved
   * EnumType itself (payload types substituted). Called by the semantic
   * analyzer when the generic enum is instantiated; codegen walks these to
   * build the storage structs.
   */
  void addSpecialization(
      DeclarationId id,
      std::shared_ptr<sun::types::EnumType> specialized) const {
    specializations_[id] = std::move(specialized);
  }
  const std::map<DeclarationId, std::shared_ptr<sun::types::EnumType>>&
  /** Provides the concrete instances created from this generic declaration. */
  getSpecializations() const {
    return specializations_;
  }

  /** Drop computed enum instances while retaining the declaration identity. */
  void clearComputedAnalysis() const override {
    ExprAST::clearComputedAnalysis();
    specializations_.clear();
  }

  /** Drop instances and annotations belonging to the discarded session. */
  void resetAnalysisSession() const override {
    ExprAST::resetAnalysisSession();
    specializations_.clear();
  }

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::ENUM_DEFINITION; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result =
        std::string(isPublic() ? "public " : "") + "enum " + name +
        (underlyingType.empty() ? "" : " " + underlyingType) + " { ";
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
      if (variants[i].hasExplicitValue) {
        result += " = " + getValueText(variants[i]);
      }
    }
    result += " }";
    return result;
  }

  /**
   * True if any variant carries a payload (tagged-union representation)
   */
  bool hasAnyPayload() const {
    for (const auto& v : variants) {
      if (v.hasPayload()) return true;
    }
    return false;
  }

  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }
  /** Provides the alternatives declared by this enum. */
  const std::vector<EnumVariantDecl>& getVariants() const { return variants; }
  /** Returns the modifiable enum alternatives. */
  std::vector<EnumVariantDecl>& getMutableVariants() { return variants; }

  /**
   * Comment written above the enum (see doc_comments.h)
   */
  const std::string& getDoc() const { return doc_; }
  /** Stores the source documentation comment for this declaration. */
  void setDoc(std::string doc) { doc_ = std::move(doc); }

  /**
   * Get a variant by name
   */
  const EnumVariantDecl* getVariant(const std::string& variantName) const {
    for (const auto& variant : variants) {
      if (variant.name == variantName) return &variant;
    }
    return nullptr;
  }

  /**
   * Get the number of variants
   */
  size_t getNumVariants() const { return variants.size(); }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Enum\n" + name; }
};

}  // namespace sun::ast
