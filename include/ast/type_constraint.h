// type_constraint.h — TypeConstraint struct for a parsed generic constraint

#pragma once

#include <optional>
#include <string>

#include "ast/type_annotation.h"
#include "semantic_analysis/qualified_name.h"
#include "support/position.h"

// One requirement written on a generic type parameter, after the colon:
//
//   <T: _Numeric>   a built-in trait (see semantic_analysis/type_traits.h)
//   <T: IShape>     an interface the type argument must implement
//   <T: IClone<T>>  an interface with type arguments
//   <F: _Lambda>    any closure type
//
// Parsing records the name, type arguments and source location. Deciding
// whether the name is a trait or an interface, and whether a given type
// argument satisfies it, is semantic analysis' job
// (`sun::traits::satisfies`), which is what lets `_is<T>` in a body and a
// constraint on a signature answer with one vocabulary.
struct TypeConstraint {
  std::string name;  // "_Numeric", "IShape", "_Lambda"
  std::optional<sun::QualifiedName> qualifiedName;
  std::vector<TypeAnnotation> typeArguments;
  Position span{};  // where it was written, for diagnostics

  TypeConstraint() = default;
  explicit TypeConstraint(std::string n) : name(std::move(n)) {}

  // Constraints are compared by what they require, not by where they appear,
  // so two spellings of the same requirement in different files are equal.
  bool operator==(const TypeConstraint& other) const {
    return (qualifiedName || other.qualifiedName
                ? qualifiedName == other.qualifiedName
                : name == other.name) &&
           typeArguments == other.typeArguments;
  }

  /** Render the complete requirement for diagnostics. */
  std::string toString() const {
    if (typeArguments.empty()) return name;
    std::string result = name + "<";
    for (size_t i = 0; i < typeArguments.size(); ++i) {
      if (i) result += ", ";
      result += typeArguments[i].toString();
    }
    return result + ">";
  }

  /** Represent the required interface using ordinary type syntax. */
  TypeAnnotation toAnnotation() const {
    TypeAnnotation result(name);
    result.qualifiedName = qualifiedName;
    result.span = span;
    for (const auto& argument : typeArguments)
      result.typeArguments.push_back(
          std::make_unique<TypeAnnotation>(argument));
    return result;
  }

  // The name semantic analysis keys the requirement by: the mangled qualified
  // name once resolution has attached one, otherwise the name as written.
  std::string resolvedName() const {
    return qualifiedName ? qualifiedName->mangled() : name;
  }
};
