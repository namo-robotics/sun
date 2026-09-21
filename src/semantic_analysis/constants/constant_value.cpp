// constant_value.cpp — Helpers for compile-time values (see constant_value.h)

#include "semantic_analysis/constants/constant_value.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>

#include "types/enum_type.h"

using SunType = sun::types::Type;

/** Evaluates constant expressions while a program is being analyzed. */
namespace sun::semantic_analysis::constants {

std::optional<unsigned> getIntegerBitWidth(const SunType& type) {
  switch (type.getKind()) {
    case SunType::Kind::Bool:
      return 1;
    case SunType::Kind::Int8:
    case SunType::Kind::UInt8:
      return 8;
    case SunType::Kind::Int16:
    case SunType::Kind::UInt16:
      return 16;
    case SunType::Kind::Int32:
    case SunType::Kind::UInt32:
    case SunType::Kind::Char:
      return 32;
    case SunType::Kind::Int64:
    case SunType::Kind::UInt64:
      return 64;
    case SunType::Kind::Enum: {
      // A payload-free enum is stored as its underlying integer.
      const auto& enumType = static_cast<const sun::types::EnumType&>(type);
      if (enumType.hasPayload() || !enumType.getUnderlyingType())
        return std::nullopt;
      return getIntegerBitWidth(*enumType.getUnderlyingType());
    }
    default:
      return std::nullopt;
  }
}

std::string ConstantValue::toDisplayString() const {
  if (isInteger()) {
    if (type && type->isBool()) return getInteger().isZero() ? "false" : "true";
    llvm::SmallString<32> text;
    getInteger().toString(text, /*Radix=*/10, /*Signed=*/!isUnsigned());
    return std::string(text);
  }
  if (isFloat()) {
    llvm::SmallVector<char, 32> text;
    getFloat().toString(text);
    return std::string(text.begin(), text.end());
  }
  if (isString()) return "\"" + getString() + "\"";
  std::string text = "[";
  for (const auto& element : getElements()) {
    if (text.size() > 1) text += ", ";
    text += element.toDisplayString();
  }
  return text + "]";
}

}  // namespace sun::semantic_analysis::constants
