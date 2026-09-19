/** Implements type computations using only explicitly supplied facts. */
#include "type_inference/type_inferer.h"

/** Pure type computations; no semantic session is available here. */
namespace sun::type_inference {
using namespace sun::semantic_analysis;
/** Helpers used only by the inference operations in this file. */
namespace {
/** Return an integer type's width, or zero for other types. */
int integerWidth(const TypePtr& type) {
  switch (type->getKind()) {
    case Type::Kind::Int8:
    case Type::Kind::UInt8:
      return 8;
    case Type::Kind::Int16:
    case Type::Kind::UInt16:
      return 16;
    case Type::Kind::Int32:
    case Type::Kind::UInt32:
      return 32;
    case Type::Kind::Int64:
    case Type::Kind::UInt64:
      return 64;
    default:
      return 0;
  }
}
/** Construct a missing-input failure without consulting external state. */
InferenceFailure missing() {
  return {InferenceFailure::Kind::MissingType, {}, {}, {}};
}
}  // namespace
TypeResult TypeInferer::number(bool integer, uint64_t magnitude, bool negative,
                               TypePtr suffix) {
  if (suffix) return suffix;
  if (!integer) return Types::Float64();
  if (magnitude <= uint64_t(INT32_MAX) + (negative ? 1 : 0))
    return Types::Int32();
  if (magnitude <= uint64_t(INT64_MAX) + (negative ? 1 : 0))
    return Types::Int64();
  return Types::UInt64();
}
TypeResult TypeInferer::numeric(const TypePtr& left, const TypePtr& right) {
  auto lhs = unwrapRef(left), rhs = unwrapRef(right);
  if (!lhs || !rhs) return missing();
  if (lhs->getKind() == rhs->getKind()) return lhs;
  int a = integerWidth(lhs), b = integerWidth(rhs);
  if (a && b) return b > a ? rhs : lhs;
  if (lhs->isFloatingPoint() && rhs->isFloatingPoint())
    return lhs->isFloat64() ? lhs : rhs;
  return lhs;
}
TypeResult TypeInferer::unary(const TypePtr& operand, bool logicalNot) {
  if (!operand) return missing();
  return logicalNot ? Types::Bool() : unwrapRef(operand);
}
TypeResult TypeInferer::reference(const TypePtr& target,
                                  bool mutableReference) {
  if (!target) return missing();
  return Types::Reference(unwrapRef(target), mutableReference);
}
TypeResult TypeInferer::array(const TypePtr& firstElement, size_t count,
                              TypePtr expectedElement,
                              bool useExpectedElement) {
  if (!count)
    return InferenceFailure{InferenceFailure::Kind::EmptyArray, {}, {}, {}};
  if (!firstElement) return missing();
  auto element =
      useExpectedElement && expectedElement && !firstElement->isArray()
          ? expectedElement
          : firstElement;
  std::vector<size_t> dimensions{count};
  if (element->isArray()) {
    const auto& inner = static_cast<const ArrayType&>(*element);
    dimensions.insert(dimensions.end(), inner.getDimensions().begin(),
                      inner.getDimensions().end());
    element = inner.getElementType();
  }
  return Types::Array(element, std::move(dimensions));
}
TypeResult TypeInferer::index(const TypePtr& target, size_t count) {
  auto type = unwrapRef(target);
  if (!type || !type->isArray())
    return InferenceFailure{InferenceFailure::Kind::NotArray, type, {}, {}};
  const auto& array = static_cast<const ArrayType&>(*type);
  if (!array.isUnsized() && array.getDimensions().size() != count)
    return InferenceFailure{
        InferenceFailure::Kind::IndexDimensions, type, {}, {}};
  return array.getElementType();
}
TypeResult TypeInferer::branches(const TypePtr& left, const TypePtr& right,
                                 bool leftToRight, bool rightToLeft) {
  if (!left || !right) return missing();
  if (left->equals(*right)) return left;
  if (leftToRight && rightToLeft) {
    if (left->isFloat64()) return left;
    if (right->isFloat64()) return right;
    return left;
  }
  if (leftToRight) return right;
  if (rightToLeft) return left;
  return InferenceFailure{
      InferenceFailure::Kind::BranchMismatch, left, right, {}};
}
TypeResult TypeInferer::call(const TypePtr& callable) {
  if (!callable) return missing();
  TypePtr result;
  if (callable->isFunction())
    result = static_cast<const FunctionType&>(*callable).getReturnType();
  else if (callable->isLambda())
    result = static_cast<const LambdaType&>(*callable).getReturnType();
  else
    return InferenceFailure{
        InferenceFailure::Kind::NotCallable, callable, {}, {}};
  if (!result) return missing();
  return result;
}
}  // namespace sun::type_inference
