// type_traits.cpp — testing a type against a trait, an interface, or a name
//
// One predicate, shared by `_is<T>(value)` in a body and by a `<T: Trait>`
// constraint on a signature, so the two can never disagree about what
// `_Numeric` means.

#include "semantic_analysis/type_traits.h"

namespace sun::semantic_analysis {

bool satisfies(const TypePtr& type, const TypePtr& requirement) {
  if (!type || !requirement) return false;

  // A borrow satisfies whatever it borrows: `ref i32` is numeric.
  TypePtr valueType = unwrapRef(type);
  if (!valueType) return false;

  auto trait =
      requirement->isTypeParameter()
          ? getTypeTrait(
                static_cast<const TypeParameterType&>(*requirement).getName())
          : TypeTrait::None;
  switch (trait) {
    case TypeTrait::Integer:
      return valueType->isSigned() || valueType->isUnsigned();
    case TypeTrait::Signed:
      return valueType->isSigned();
    case TypeTrait::Unsigned:
      return valueType->isUnsigned();
    case TypeTrait::Float:
      return valueType->isFloat32() || valueType->isFloat64();
    case TypeTrait::Numeric:
      return valueType->isNumeric();
    case TypeTrait::Primitive:
      return valueType->isPrimitive();
    case TypeTrait::Lambda:
      return valueType->isLambda();
    case TypeTrait::Function:
      return valueType->isFunction();
    case TypeTrait::Callable:
      return valueType->isLambda() || valueType->isFunction();
    case TypeTrait::None:
      break;
  }

  if (valueType->isClass() && requirement->isInterface()) {
    const auto& classType = static_cast<const ClassType&>(*valueType);
    return classType.implementsInterface(
        static_cast<const InterfaceType&>(*requirement));
  }
  return valueType->equals(*requirement);
}

}  // namespace sun::semantic_analysis
