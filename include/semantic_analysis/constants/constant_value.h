// constant_value.h — A value the compiler computed from a constant expression.

#pragma once

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "types/types.h"

/** Evaluates constant expressions while a program is being analyzed. */
namespace sun::semantic_analysis::constants {

/**
 * A value known at compile time, together with the Sun type it has.
 *
 * Integers, bools, chars and payload-free enum variants are all held as an
 * integer whose width is the width the type has in generated code; whether it
 * is signed is a property of the type, not of the stored bits. Floats keep
 * their own precision. An array holds one value per element, and nests for
 * more than one dimension.
 */
struct ConstantValue {
  /** The elements of an array value, outermost dimension first. */
  using Elements = std::vector<ConstantValue>;

  sun::types::TypePtr type;
  std::variant<llvm::APInt, llvm::APFloat, std::string, Elements> data;

  /** Reports whether the value is held as an integer. */
  bool isInteger() const { return std::holds_alternative<llvm::APInt>(data); }
  /** Reports whether the value is a floating-point number. */
  bool isFloat() const { return std::holds_alternative<llvm::APFloat>(data); }
  /** Reports whether the value is the text of a string literal. */
  bool isString() const { return std::holds_alternative<std::string>(data); }
  /** Reports whether the value is an array of values. */
  bool isArray() const { return std::holds_alternative<Elements>(data); }

  /** The integer bits; the value must be held as an integer. */
  const llvm::APInt& getInteger() const { return std::get<llvm::APInt>(data); }
  /** The floating-point number; the value must be a float. */
  const llvm::APFloat& getFloat() const {
    return std::get<llvm::APFloat>(data);
  }
  /** The text; the value must be a string. */
  const std::string& getString() const { return std::get<std::string>(data); }
  /** The elements; the value must be an array. */
  const Elements& getElements() const { return std::get<Elements>(data); }

  /**
   * Reports whether an integer value reads as unsigned, which is decided by
   * its type exactly as generated code decides it.
   */
  bool isUnsigned() const { return type && type->isUnsigned(); }

  /** Renders the value for diagnostics and tests, such as `-4` or `[1, 2]`. */
  std::string toDisplayString() const;
};

/**
 * Gives a value read from a bundle the type of the global it belongs to, and
 * its elements the types that follow from it. A bundle stores values without
 * types. Returns nothing when the value does not have the shape of the type,
 * which means the bundle and its metadata disagree.
 */
std::optional<ConstantValue> adoptType(ConstantValue value,
                                       const sun::types::TypePtr& type);

/**
 * Number of bits generated code uses for a value of this type, or nothing
 * when the type is not held as an integer.
 */
std::optional<unsigned> getIntegerBitWidth(const sun::types::Type& type);

}  // namespace sun::semantic_analysis::constants
