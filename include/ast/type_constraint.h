// type_constraint.h — TypeConstraint struct for a parsed generic constraint

#pragma once

#include <string>

#include "support/position.h"

// One requirement written on a generic type parameter, after the colon:
//
//   <T: _Numeric>   a built-in trait (see semantic_analysis/type_traits.h)
//   <T: IShape>     an interface the type argument must implement
//   <F: lambda>     any closure type
//
// Parsing keeps this purely syntactic — it records the name as written and
// where it was written. Deciding what the name refers to, and whether a given
// type argument satisfies it, is semantic analysis' job
// (`sun::traits::satisfies`), which is what lets `_is<T>` in a body and a
// constraint on a signature answer with one vocabulary.
struct TypeConstraint {
  std::string name;   // "_Numeric", "IShape", "lambda"
  Position span{};    // where it was written, for diagnostics

  TypeConstraint() = default;
  explicit TypeConstraint(std::string n) : name(std::move(n)) {}
  TypeConstraint(std::string n, Position s)
      : name(std::move(n)), span(std::move(s)) {}

  // Constraints are compared by what they require, not by where they appear,
  // so two spellings of the same requirement in different files are equal.
  bool operator==(const TypeConstraint& other) const {
    return name == other.name;
  }

  std::string toString() const { return name; }
};
