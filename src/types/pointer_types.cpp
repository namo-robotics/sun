/** Implements raw pointer compatibility without implicit ownership changes. */
#include "types/pointer_types.h"

/** Implements shared type descriptions and queries. */
namespace sun::types {
bool RawPointerType::equals(const Type& other) const {
  // Raw pointer is compatible with null
  if (other.isNullPointer()) return true;
  // NOT compatible with static_ptr in either direction here: equals is used
  // symmetrically, and only one direction is sound. The static_ptr → raw_ptr
  // narrowing lives in the explicit conversion rules (isAssignableTo, the
  // overload matchers, and analyzeCall).
  if (auto* p = dynamic_cast<const RawPointerType*>(&other)) {
    return pointeeType->equals(*p->pointeeType);
  }
  return false;
}

}  // namespace sun::types
