// qualified_name.cpp — Centralized name mangling implementation

#include "qualified_name.h"

#include "types.h"

namespace sun {

std::string QualifiedName::canonicalTypeString(const TypePtr& type,
                                               const std::string& hashPrefix) {
  if (!type) return "void";

  if (type->isClass()) {
    std::string typeMangledName =
        static_cast<const ClassType*>(type.get())->getMangledName();
    // Strip hash prefix only if it matches our library's hash
    if (!hashPrefix.empty() && typeMangledName.size() > hashPrefix.size() &&
        typeMangledName.substr(0, hashPrefix.size()) == hashPrefix) {
      typeMangledName = typeMangledName.substr(hashPrefix.size());
    }
    return typeMangledName;
  }

  if (type->isReference()) {
    auto* refType = static_cast<const ReferenceType*>(type.get());
    std::string inner =
        canonicalTypeString(refType->getReferencedType(), hashPrefix);
    return "ref_" + inner + "_";
  }

  if (type->isRawPointer()) {
    auto* ptrType = static_cast<const RawPointerType*>(type.get());
    std::string inner =
        canonicalTypeString(ptrType->getPointeeType(), hashPrefix);
    return "raw_ptr_" + inner + "_";
  }

  if (type->isStaticPointer()) {
    auto* ptrType = static_cast<const StaticPointerType*>(type.get());
    std::string inner =
        canonicalTypeString(ptrType->getPointeeType(), hashPrefix);
    return "static_ptr_" + inner + "_";
  }

  if (type->isArray()) {
    auto* arrType = static_cast<const ArrayType*>(type.get());
    std::string inner =
        canonicalTypeString(arrType->getElementType(), hashPrefix);
    return "array_" + inner + "_";
  }

  // For primitives and other types, toString() is stable
  return type->toString();
}

std::string QualifiedName::buildParamSuffix(
    const std::vector<TypePtr>& paramTypes, const std::string& hashPrefix) {
  if (paramTypes.empty()) return "";

  std::string result;
  for (const auto& paramType : paramTypes) {
    result += "$";
    std::string typeStr = canonicalTypeString(paramType, hashPrefix);
    // Sanitize: replace special chars that may cause issues in symbol names
    for (char& c : typeStr) {
      if (c == '<' || c == '>' || c == ',' || c == '(' || c == ')') c = '_';
      if (c == ' ') c = '_';
    }
    result += typeStr;
  }
  return result;
}

}  // namespace sun
