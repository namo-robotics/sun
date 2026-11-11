/** Checks structural ownership obligations while retaining exact identities. */
#include "abi/correspondence_checker.h"

#include "abi/abi_compatibility_checker.h"

/** Checks correspondence between independently compiled declarations. */
namespace sun::abi {
std::string CorrespondenceResult::explanation() const {
  std::string result;
  for (const auto& component : path) {
    if (!result.empty()) {
      result += " -> ";
    }
    result += component;
  }
  if (!result.empty()) {
    result += ": ";
  }
  return result + reason;
}

CorrespondenceResult CorrespondenceResult::within(
    const std::string& component) const {
  auto result = *this;
  if (!proven) {
    result.path.insert(result.path.begin(), component);
  }
  return result;
}

CorrespondenceChecker::CorrespondenceChecker(
    const AbiCompatibilityChecker& imports)
    : imports_(imports) {}

CorrespondenceResult CorrespondenceChecker::checkBorrow(
    const types::TypePtr& source, const types::TypePtr& target) const {
  // A receiver can call destination methods even through a const borrow.
  // Until a narrower observation proof exists, require the same invariants
  // as a move. Reference mutability and lifetime are checked by the caller.
  return checkOwnedTransfer(source, target);
}

CorrespondenceResult CorrespondenceChecker::checkArguments(
    const std::vector<types::TypePtr>& source,
    const std::vector<types::TypePtr>& target) const {
  if (source.size() != target.size()) {
    return {false, "generic argument counts differ"};
  }
  for (size_t i = 0; i < source.size(); ++i) {
    auto result = checkOwnedTransfer(source[i], target[i]);
    if (!result.proven) {
      return result.within("generic argument " + std::to_string(i + 1));
    }
  }
  return {true};
}

CorrespondenceResult CorrespondenceChecker::checkStorage(
    const types::TypePtr& source, const types::TypePtr& target) const {
  if (!source || !target) {
    return {false, "storage type is unresolved"};
  }
  if (source->isRawPointer() || source->isReference() || source->isFunction() ||
      source->isLambda() || source->isInterface()) {
    return {false, "pointer ownership or lifetime contract is not established"};
  }
  return checkOwnedTransfer(source, target);
}

CorrespondenceResult CorrespondenceChecker::checkOwnedTransfer(
    const types::TypePtr& source, const types::TypePtr& target) const {
  if (!source || !target) {
    return {false, "type is unresolved"};
  }
  if (source->equals(*target)) {
    return {true};
  }
  if (source->isClass() && target->isClass()) {
    const auto& from = static_cast<const types::ClassType&>(*source);
    const auto& to = static_cast<const types::ClassType&>(*target);
    const auto name = source->toDisplayString();
    if (from.isSpecialized() != to.isSpecialized() ||
        !imports_.hasCompatibleNominalCorrespondence(
            from.isSpecialized() ? from.getGenericQualifiedName()
                                 : from.getQualifiedName(),
            to.isSpecialized() ? to.getGenericQualifiedName()
                               : to.getQualifiedName(),
            NominalKind::Class)) {
      return {false,
              "no compatible named-library declaration correspondence",
              {name}};
    }
    auto arguments =
        checkArguments(from.getTypeArguments(), to.getTypeArguments());
    if (!arguments.proven) {
      return arguments.within(name);
    }
    const bool hasCleanup = from.getMethod("deinit") ||
                            to.getMethod("deinit") || from.deinitializer ||
                            to.deinitializer;
    if (from.isPacked() != to.isPacked() ||
        from.getAlignment() != to.getAlignment() ||
        from.getFields().size() != to.getFields().size()) {
      return {false, "class storage layouts differ", {name}};
    }
    for (size_t i = 0; i < from.getFields().size(); ++i) {
      const auto& a = from.getFields()[i];
      const auto& b = to.getFields()[i];
      if (a.name != b.name || a.index != b.index) {
        return {false, "field order or identity differs", {name, a.name}};
      }
      auto result = checkStorage(a.type, b.type);
      if (!result.proven) {
        return result.within(a.name).within(name);
      }
    }
    return imports_
        .compareNominalImplementation(
            from.isSpecialized() ? from.getGenericQualifiedName()
                                 : from.getQualifiedName(),
            to.isSpecialized() ? to.getGenericQualifiedName()
                               : to.getQualifiedName(),
            NominalKind::Class, !hasCleanup)
        .within(name);
  }
  if (source->isEnum() && target->isEnum()) {
    const auto& from = static_cast<const types::EnumType&>(*source);
    const auto& to = static_cast<const types::EnumType&>(*target);
    const auto name = source->toDisplayString();
    if (from.isGenericSpecialization() != to.isGenericSpecialization() ||
        !imports_.hasCompatibleNominalCorrespondence(from.getQualifiedName(),
                                                     to.getQualifiedName(),
                                                     NominalKind::Enum)) {
      return {false,
              "no compatible named-library declaration correspondence",
              {name}};
    }
    auto arguments = checkArguments(from.getGenericArgs(), to.getGenericArgs());
    if (!arguments.proven) {
      return arguments.within(name);
    }
    if (!from.getUnderlyingType()->equals(*to.getUnderlyingType()) ||
        from.getVariants().size() != to.getVariants().size()) {
      return {false, "enum tag or variant layout differs", {name}};
    }
    for (size_t i = 0; i < from.getVariants().size(); ++i) {
      const auto& a = from.getVariants()[i];
      const auto& b = to.getVariants()[i];
      if (a.name != b.name || a.value != b.value ||
          a.payloadTypes.size() != b.payloadTypes.size()) {
        return {false, "enum variant layout differs", {name, a.name}};
      }
      for (size_t j = 0; j < a.payloadTypes.size(); ++j) {
        auto result = checkStorage(a.payloadTypes[j], b.payloadTypes[j]);
        if (!result.proven) {
          return result.within(a.name).within(name);
        }
      }
    }
    return imports_
        .compareNominalImplementation(from.getQualifiedName(),
                                      to.getQualifiedName(), NominalKind::Enum,
                                      true)
        .within(name);
  }
  return {false, "types have different identities or representations"};
}
}  // namespace sun::abi
