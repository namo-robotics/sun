/** Declares exact type identity comparisons without conversions. */
#pragma once

#include <vector>

#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/** Compare semantic types exactly, without accepting implicit conversions. */
bool sameTypeIdentity(const TypePtr& left, const TypePtr& right);
/** Compare ordered parameter or specialization argument types exactly. */
bool sameTypeArguments(const std::vector<TypePtr>& left,
                       const std::vector<TypePtr>& right);

}  // namespace sun::types
