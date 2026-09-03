// The one place that says how a call argument reaches its parameter. See
// include/semantic_analysis/argument_conversion.h.

#include "semantic_analysis/argument_conversion.h"

#include "semantic_analysis/generic_type_arguments.h"
#include "support/error.h"

namespace sun::conversions {

namespace {

// Bit width of a numeric primitive; 0 for anything else.
int numericBits(const Type& type) {
  switch (type.getKind()) {
    case Type::Kind::Int8:
    case Type::Kind::UInt8:
      return 8;
    case Type::Kind::Int16:
    case Type::Kind::UInt16:
      return 16;
    case Type::Kind::Int32:
    case Type::Kind::UInt32:
    case Type::Kind::Float32:
      return 32;
    case Type::Kind::Int64:
    case Type::Kind::UInt64:
    case Type::Kind::Float64:
      return 64;
    default:
      return 0;
  }
}

bool isFloat(const Type& type) {
  return type.getKind() == Type::Kind::Float32 ||
         type.getKind() == Type::Kind::Float64;
}

bool isPayloadEnum(const TypePtr& type) {
  return type && type->isEnum() &&
         static_cast<const EnumType*>(type.get())->hasPayload();
}

// A value handed over as itself: an owning compound moves, anything else is
// passed as is. A value read out of a borrow never moves (only types that copy
// by read get this far, checked by isAssignableTo).
ArgConversion byValue(const TypePtr& argType) {
  if (typeMovesOnRead(argType)) return ArgConversion::Move;
  return ArgConversion::PassValue;
}

// True if `target` is an unsized array<T> and `value` a sized array of the
// same element type: the argument's storage is viewed with its rank erased.
bool decaysToView(const TypePtr& value, const TypePtr& target) {
  if (!value || !target || !value->isArray() || !target->isArray())
    return false;
  auto* valueArray = static_cast<const ArrayType*>(value.get());
  auto* targetArray = static_cast<const ArrayType*>(target.get());
  return targetArray->isUnsized() && !valueArray->isUnsized() &&
         targetArray->getElementType()->equals(*valueArray->getElementType());
}

}  // namespace

const char* toString(ArgConversion conversion) {
  switch (conversion) {
    case ArgConversion::PassValue:
      return "pass by value";
    case ArgConversion::Move:
      return "move";
    case ArgConversion::Borrow:
      return "borrow";
    case ArgConversion::ArrayToView:
      return "array to view";
    case ArgConversion::RawPtrAsRef:
      return "raw pointer as reference";
    case ArgConversion::ClassToInterface:
      return "class to interface";
    case ArgConversion::BorrowedClassToInterface:
      return "borrowed class to interface";
    case ArgConversion::ClassToRefInterface:
      return "class to ref interface";
    case ArgConversion::WidenNumeric:
      return "numeric widening";
    case ArgConversion::StaticToRawPtr:
      return "static_ptr to raw_ptr";
    case ArgConversion::DerefRawPtr:
      return "raw pointer auto-deref";
    case ArgConversion::CVararg:
      return "C vararg promotion";
  }
  return "?";
}

std::optional<ArgConversion> classifyArgument(const TypePtr& argType,
                                              const TypePtr& paramType,
                                              bool cVariadicTail) {
  if (!argType) return std::nullopt;

  // Past the declared parameters: a C `...` tail, or a variadic pack
  if (!paramType) {
    return cVariadicTail ? ArgConversion::CVararg : byValue(argType);
  }

  // A template body is analyzed with its type parameters unbound; the real
  // decision is made when it is instantiated.
  if (generics::mentionsTypeParameter(argType) ||
      generics::mentionsTypeParameter(paramType)) {
    return ArgConversion::PassValue;
  }

  TypePtr value = unwrapRef(argType);

  // `ref T` and `const ref T` parameters take the argument's address alike;
  // constness is checked by semantic analysis and has no lowering of its own.
  if (paramType->isReference()) {
    TypePtr target = unwrapRef(paramType);
    if (argType->isRawPointer() && target) {
      const TypePtr& pointee =
          static_cast<const RawPointerType*>(argType.get())->getPointeeType();
      if (pointee && pointee->equals(*target))
        return ArgConversion::RawPtrAsRef;
    }
    if (value && value->isClass() && target && target->isInterface()) {
      return ArgConversion::ClassToRefInterface;
    }
    if (decaysToView(value, target)) return ArgConversion::ArrayToView;
    return ArgConversion::Borrow;
  }

  if (paramType->isInterface() && value && value->isClass()) {
    return argType->isReference()
               ? ArgConversion::BorrowedClassToInterface
               : ArgConversion::ClassToInterface;
  }

  if (argType->isStaticPointer() && paramType->isRawPointer()) {
    return ArgConversion::StaticToRawPtr;
  }

  if (argType->isRawPointer() && paramType->isPrimitive()) {
    const TypePtr& pointee =
        static_cast<const RawPointerType*>(argType.get())->getPointeeType();
    if (pointee && pointee->equals(*paramType))
      return ArgConversion::DerefRawPtr;
  }

  if (value && paramType->equals(*value)) return byValue(argType);

  // Numeric: only widening has a lowering. Same-width signedness changes
  // share a representation; a narrowing or an int/float mix has no lowering,
  // and acceptance should not have let it through.
  int argBits = value ? numericBits(*value) : 0;
  int paramBits = numericBits(*paramType);
  if (argBits && paramBits) {
    if (isFloat(*value) != isFloat(*paramType)) return std::nullopt;
    if (argBits < paramBits) return ArgConversion::WidenNumeric;
    if (argBits == paramBits) return ArgConversion::PassValue;
    return std::nullopt;
  }

  // Everything else that is accepted shares a representation with the
  // parameter: null to a pointer, raw_ptr<T> to a byte pointer, a
  // non-throwing lambda for a throwing one, a class seen through two type
  // instances.
  return byValue(argType);
}

std::vector<ArgConversion> classifyArguments(
    const std::vector<TypePtr>& argTypes,
    const std::vector<TypePtr>& paramTypes, bool cVariadic,
    const std::string& calleeName, std::optional<Position> loc) {
  std::vector<ArgConversion> conversions;
  conversions.reserve(argTypes.size());
  for (size_t i = 0; i < argTypes.size(); ++i) {
    TypePtr paramType = i < paramTypes.size() ? paramTypes[i] : nullptr;
    bool tail = !paramType && cVariadic;
    auto conversion = classifyArgument(argTypes[i], paramType, tail);
    if (!conversion) {
      logAndThrowError(
          "Type mismatch in argument " + std::to_string(i + 1) +
              " of call to '" + calleeName + "': expected " +
              (paramType ? paramType->toDisplayString() : "?") + ", got " +
              (argTypes[i] ? argTypes[i]->toDisplayString() : "?") +
              " (no conversion narrows a number; use _convert<T>() or "
              "safe_convert<T>())",
          loc);
    }
    conversions.push_back(*conversion);
  }
  return conversions;
}

}  // namespace sun::conversions
