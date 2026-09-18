#include "semantic_analysis/types.h"

namespace sun {
namespace {

/** Compare identity recursively without accepting implicit conversions. */
bool sameType(const TypePtr& left, const TypePtr& right) {
  if (left == right) return true;
  if (!left || !right || left->getKind() != right->getKind()) return false;
  auto parameters = [](const auto& a, const auto& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (!sameType(a[i], b[i])) return false;
    return true;
  };
  auto callable = [&](const auto& a, const auto& b) {
    return a.canThrow() == b.canThrow() &&
           a.requiresUnsafe() == b.requiresUnsafe() &&
           sameType(a.getReturnType(), b.getReturnType()) &&
           parameters(a.getParamTypes(), b.getParamTypes());
  };
  switch (left->getKind()) {
    case Type::Kind::Reference: {
      const auto& a = static_cast<const ReferenceType&>(*left);
      const auto& b = static_cast<const ReferenceType&>(*right);
      return a.isMutable() == b.isMutable() &&
             sameType(a.getReferencedType(), b.getReferencedType());
    }
    case Type::Kind::RawPointer:
      return sameType(
          static_cast<const RawPointerType&>(*left).getPointeeType(),
          static_cast<const RawPointerType&>(*right).getPointeeType());
    case Type::Kind::StaticPointer:
      return sameType(
          static_cast<const StaticPointerType&>(*left).getPointeeType(),
          static_cast<const StaticPointerType&>(*right).getPointeeType());
    case Type::Kind::Array: {
      const auto& a = static_cast<const ArrayType&>(*left);
      const auto& b = static_cast<const ArrayType&>(*right);
      return a.getDimensions() == b.getDimensions() &&
             sameType(a.getElementType(), b.getElementType());
    }
    case Type::Kind::Function:
      return callable(static_cast<const FunctionType&>(*left),
                      static_cast<const FunctionType&>(*right));
    case Type::Kind::Lambda: {
      const auto& a = static_cast<const LambdaType&>(*left);
      const auto& b = static_cast<const LambdaType&>(*right);
      return a.hasRefCaptures() == b.hasRefCaptures() && callable(a, b);
    }
    case Type::Kind::ErrorUnion:
      return sameType(
          static_cast<const ErrorUnionType&>(*left).getValueType(),
          static_cast<const ErrorUnionType&>(*right).getValueType());
    default:
      return left->equals(*right);
  }
}

/** Compare concrete argument lists in their declared order. */
bool sameArguments(const std::vector<TypePtr>& left,
                   const std::vector<TypePtr>& right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i)
    if (!sameType(left[i], right[i])) return false;
  return true;
}

}  // namespace

bool SpecializationKey::operator==(const SpecializationKey& other) const {
  return source == other.source && enclosing == other.enclosing &&
         sameArguments(arguments, other.arguments) &&
         variadic.has_value() == other.variadic.has_value() &&
         (!variadic || sameArguments(*variadic, *other.variadic));
}

}  // namespace sun
