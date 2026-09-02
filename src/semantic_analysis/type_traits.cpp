// type_traits.cpp — testing a type against a trait, an interface, or a name
//
// One predicate, shared by `_is<T>(value)` in a body and by a `<T: Trait>`
// constraint on a signature, so the two can never disagree about what
// `_Numeric` means.

#include "semantic_analysis/type_traits.h"

namespace sun::traits {

bool satisfies(const TypePtr& type, const std::string& name) {
  if (!type) return false;

  // A borrow satisfies whatever it borrows: `ref i32` is numeric.
  TypePtr valueType = unwrapRef(type);
  if (!valueType) return false;

  switch (getTypeTrait(name)) {
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

  // Not a built-in trait. A class may implement it as an interface, or the
  // name may be the type's own — both spellings of "is exactly this".
  if (valueType->isClass()) {
    auto* classType = static_cast<ClassType*>(valueType.get());
    return classType->implementsInterface(name) ||
           classType->getMangledName() == name;
  }
  return valueType->toString() == name;
}

}  // namespace sun::traits
