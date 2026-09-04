// qualified_name.cpp — Centralized name mangling implementation

#include "semantic_analysis/qualified_name.h"

#include "semantic_analysis/types.h"

namespace sun {

namespace {

// The raw spelling of a type, before it is made safe for a symbol name
std::string spellType(const TypePtr& type) {
  if (!type) return "void";

  // Classes and interfaces go in by their mangled name, library hash and
  // all: a bundle and its importers spell that name identically, so the
  // same signature reads the same on both sides.
  if (type->isClass()) {
    return static_cast<const ClassType*>(type.get())->getMangledName();
  }
  if (type->isInterface()) {
    return static_cast<const InterfaceType*>(type.get())->getName();
  }

  if (type->isReference()) {
    auto* refType = static_cast<const ReferenceType*>(type.get());
    std::string inner = spellType(refType->getReferencedType());
    // Mutability is part of the type: Option<ref T> and Option<const ref T>
    // are different specializations
    return (refType->isMutable() ? "ref_" : "const_ref_") + inner + "_";
  }

  if (type->isRawPointer()) {
    auto* ptrType = static_cast<const RawPointerType*>(type.get());
    std::string inner = spellType(ptrType->getPointeeType());
    return "raw_ptr_" + inner + "_";
  }

  if (type->isStaticPointer()) {
    auto* ptrType = static_cast<const StaticPointerType*>(type.get());
    std::string inner = spellType(ptrType->getPointeeType());
    return "static_ptr_" + inner + "_";
  }

  if (type->isArray()) {
    auto* arrType = static_cast<const ArrayType*>(type.get());
    std::string inner = spellType(arrType->getElementType());
    // The dimensions are part of the type: array<i32, 3> and array<i32, 5>
    // are different values with different sizes
    std::string dims;
    for (size_t dim : arrType->getDimensions()) {
      dims += (dims.empty() ? "" : "x") + std::to_string(dim);
    }
    if (dims.empty()) return "array_" + inner + "_";
    return "array_" + inner + "_" + dims + "_";
  }

  if (type->isLambda()) {
    auto* lamType = static_cast<const LambdaType*>(type.get());
    std::string result = lamType->hasRefCaptures() ? "<'_>(" : "(";
    const auto& params = lamType->getParamTypes();
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0) result += ", ";
      result += spellType(params[i]);
    }
    result += ") -> ";
    result += spellType(lamType->getReturnType());
    if (lamType->canThrow()) result += " throws IError";
    return result;
  }

  // For primitives and other types, toString() is stable
  return type->toString();
}

}  // namespace

std::string QualifiedName::canonicalTypeString(const TypePtr& type) {
  std::string spelled = spellType(type);
  // Punctuation a type spelling may contain that has no place in a symbol
  for (char& c : spelled) {
    if (c == '<' || c == '>' || c == ',' || c == '(' || c == ')') c = '_';
    if (c == '[' || c == ']' || c == ' ') c = '_';
  }
  return spelled;
}

std::string QualifiedName::buildParamSuffix(
    const std::vector<TypePtr>& paramTypes) {
  std::string result;
  for (const auto& paramType : paramTypes) {
    result += "$" + canonicalTypeString(paramType);
  }
  return result;
}

std::string QualifiedName::buildVariadicArgSuffix(
    const std::vector<TypePtr>& variadicArgTypes) {
  if (variadicArgTypes.empty()) return "";

  std::string result = "$v$";
  for (const auto& argType : variadicArgTypes) {
    result += "$" + canonicalTypeString(argType);
  }
  return result;
}

QualifiedName QualifiedName::specializationOf(
    const QualifiedName& templateName, const std::vector<TypePtr>& typeArgs,
    const std::vector<TypePtr>& packArgTypes) {
  QualifiedName result = templateName;
  for (const auto& typeArg : typeArgs) {
    result.baseName += "_" + canonicalTypeString(typeArg);
  }
  // A pack's element types are part of the specialization's identity too, so
  // two call sites at different arities get two functions rather than
  // colliding on whichever was instantiated first.
  result.baseName += buildVariadicArgSuffix(packArgTypes);
  // The type arguments already tell the specializations apart.
  result.paramSuffix.clear();
  return result;
}

}  // namespace sun
