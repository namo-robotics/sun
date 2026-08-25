// Inferring the type arguments of a generic call from its arguments. See
// include/semantic_analysis/generic_type_arguments.h for the shapes that are
// matched.

#include "semantic_analysis/generic_type_arguments.h"

#include <algorithm>

#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

namespace sun::generics {

namespace {

bool isNamed(const std::vector<std::string>& typeParams,
             const std::string& name) {
  return std::find(typeParams.begin(), typeParams.end(), name) !=
         typeParams.end();
}

// Reading through a borrow gives the referent's type everywhere but under a
// `ref` parameter, which binds against the referent too.
TypePtr referent(const TypePtr& type) {
  if (type && type->isReference()) {
    return static_cast<const ReferenceType*>(type.get())->getReferencedType();
  }
  return type;
}

// The element a pointer or array parameter binds against.
TypePtr elementOf(const TypePtr& type) {
  if (type->isArray()) {
    return static_cast<const ArrayType*>(type.get())->getElementType();
  }
  if (type->isRawPointer()) {
    return static_cast<const RawPointerType*>(type.get())->getPointeeType();
  }
  if (type->isStaticPointer()) {
    return static_cast<const StaticPointerType*>(type.get())->getPointeeType();
  }
  return nullptr;
}

// The type arguments of a generic class or enum value.
const std::vector<TypePtr>* typeArgumentsOf(const TypePtr& type) {
  if (type->isClass()) {
    return &static_cast<const ClassType*>(type.get())->getTypeArguments();
  }
  if (type->isEnum()) {
    return &static_cast<const EnumType*>(type.get())->getGenericArgs();
  }
  if (type->isInterface()) {
    return &static_cast<const InterfaceType*>(type.get())->getTypeArguments();
  }
  return nullptr;
}

// Not named `bind`: argument-dependent lookup would pick std::bind for a
// std::string/shared_ptr/map argument list and silently build a binder.
void bindName(const std::string& name, const TypePtr& value,
              std::map<std::string, TypePtr>& bindings) {
  if (!value) return;
  auto existing = bindings.find(name);
  if (existing == bindings.end()) bindings[name] = value;
}

}  // namespace

void bindTypeParameters(const TypeAnnotation& param, const TypePtr& argType,
                        const std::vector<std::string>& typeParams,
                        std::map<std::string, TypePtr>& bindings) {
  if (!argType) return;

  // `ref T` against an argument of type T (or ref T)
  if (param.baseName == "ref" && param.elementType) {
    bindTypeParameters(*param.elementType, referent(argType), typeParams,
                       bindings);
    return;
  }

  TypePtr value = referent(argType);
  if (!value) return;

  // The parameter is the type parameter itself: T, bound to the argument
  if (param.typeArguments.empty() && isNamed(typeParams, param.baseName)) {
    bindName(param.baseName, value, bindings);
    return;
  }

  // array<T> / raw_ptr<T> / static_ptr<T>: bind against the element
  if (param.elementType) {
    if (TypePtr element = elementOf(value)) {
      bindTypeParameters(*param.elementType, element, typeParams, bindings);
    }
    return;
  }

  // Vec<T>, Map<K, V>, Option<T>: bind against the argument's type arguments
  if (!param.typeArguments.empty()) {
    const std::vector<TypePtr>* args = typeArgumentsOf(value);
    if (!args) return;
    for (size_t i = 0; i < param.typeArguments.size() && i < args->size();
         ++i) {
      bindTypeParameters(*param.typeArguments[i], (*args)[i], typeParams,
                         bindings);
    }
  }
}

void bindTypeParameters(const TypePtr& param, const TypePtr& argType,
                        const std::vector<std::string>& typeParams,
                        std::map<std::string, TypePtr>& bindings) {
  if (!param || !argType) return;

  // `ref T` against an argument of type T (or ref T)
  if (param->isReference()) {
    bindTypeParameters(referent(param), referent(argType), typeParams,
                       bindings);
    return;
  }

  TypePtr value = referent(argType);
  if (!value) return;

  if (param->isTypeParameter()) {
    const auto& name =
        static_cast<const TypeParameterType*>(param.get())->getName();
    if (isNamed(typeParams, name)) bindName(name, value, bindings);
    return;
  }

  // array<T> / raw_ptr<T> / static_ptr<T>: bind against the element
  if (TypePtr paramElement = elementOf(param)) {
    if (TypePtr element = elementOf(value)) {
      bindTypeParameters(paramElement, element, typeParams, bindings);
    }
    return;
  }

  // (A) T, (A, B) T: bind a lambda parameter's signature
  if (param->isLambda() && value->isLambda()) {
    auto* p = static_cast<const LambdaType*>(param.get());
    auto* a = static_cast<const LambdaType*>(value.get());
    const auto& pParams = p->getParamTypes();
    const auto& aParams = a->getParamTypes();
    for (size_t i = 0; i < pParams.size() && i < aParams.size(); ++i) {
      bindTypeParameters(pParams[i], aParams[i], typeParams, bindings);
    }
    bindTypeParameters(p->getReturnType(), a->getReturnType(), typeParams,
                       bindings);
    return;
  }

  // Vec<T>, Map<K, V>, Option<T>: bind against the argument's type arguments
  const std::vector<TypePtr>* paramArgs = typeArgumentsOf(param);
  const std::vector<TypePtr>* args =
      paramArgs ? typeArgumentsOf(value) : nullptr;
  if (!paramArgs || !args) return;
  for (size_t i = 0; i < paramArgs->size() && i < args->size(); ++i) {
    bindTypeParameters((*paramArgs)[i], (*args)[i], typeParams, bindings);
  }
}

bool mentionsTypeParameter(const TypePtr& type) {
  if (!type) return false;
  if (type->isTypeParameter()) return true;
  if (type->isReference()) return mentionsTypeParameter(referent(type));
  if (TypePtr element = elementOf(type)) return mentionsTypeParameter(element);
  if (type->isLambda()) {
    auto* l = static_cast<const LambdaType*>(type.get());
    return std::any_of(l->getParamTypes().begin(), l->getParamTypes().end(),
                       mentionsTypeParameter) ||
           mentionsTypeParameter(l->getReturnType());
  }
  if (const std::vector<TypePtr>* args = typeArgumentsOf(type)) {
    return std::any_of(args->begin(), args->end(), mentionsTypeParameter);
  }
  return false;
}

namespace {

// Turn the bindings into the full type-argument list, in declaration order.
// A type argument written at the call site is the caller's choice: it
// replaces whatever the arguments suggested, and an argument that disagrees
// with it is reported when the specialization is type-checked, not papered
// over by inference. A binding may still be a type parameter — the call sits
// in a template body; the caller decides whether it can specialize.
std::vector<TypePtr> completeTypeArguments(
    const std::vector<std::string>& typeParams,
    std::map<std::string, TypePtr>& bindings,
    const std::vector<TypePtr>& explicitTypeArgs, const std::string& what,
    const std::string& displayName, std::optional<Position> loc) {
  for (size_t i = 0; i < explicitTypeArgs.size() && i < typeParams.size();
       ++i) {
    bindings[typeParams[i]] = explicitTypeArgs[i];
  }

  std::vector<TypePtr> typeArgs;
  for (const auto& typeParam : typeParams) {
    auto found = bindings.find(typeParam);
    if (found == bindings.end() || !found->second) {
      logAndThrowError("Cannot infer type argument '" + typeParam + "' of " +
                           what + " '" + displayName +
                           "' from the arguments. Give it explicitly, e.g. " +
                           displayName + "<i32>(...).",
                       loc);
    }
    typeArgs.push_back(found->second);
  }
  return typeArgs;
}

}  // namespace

std::vector<TypePtr> inferGenericTypeArguments(
    const GenericFunctionInfo& genericInfo,
    const std::vector<TypePtr>& argTypes, const std::string& displayName,
    std::optional<Position> loc, const std::vector<TypePtr>& explicitTypeArgs) {
  std::map<std::string, TypePtr> bindings;
  for (size_t i = 0; i < genericInfo.params.size() && i < argTypes.size();
       ++i) {
    bindTypeParameters(genericInfo.params[i].second, argTypes[i],
                       genericInfo.typeParameters, bindings);
  }
  return completeTypeArguments(genericInfo.typeParameters, bindings,
                               explicitTypeArgs, "generic function",
                               displayName, loc);
}

std::vector<TypePtr> inferMethodTypeArguments(
    const ClassMethod& method, const std::vector<TypePtr>& argTypes,
    const std::string& displayName, std::optional<Position> loc,
    const std::vector<TypePtr>& explicitTypeArgs) {
  std::map<std::string, TypePtr> bindings;
  for (size_t i = 0; i < method.paramTypes.size() && i < argTypes.size(); ++i) {
    bindTypeParameters(method.paramTypes[i], argTypes[i], method.typeParameters,
                       bindings);
  }
  return completeTypeArguments(method.typeParameters, bindings,
                               explicitTypeArgs, "generic method", displayName,
                               loc);
}

}  // namespace sun::generics

sun::TypePtr SemanticAnalyzer::genericFunctionSignature(
    const GenericFunctionInfo& genericInfo,
    const std::vector<sun::TypePtr>& typeArgs) {
  enterTypeParamScope(genericInfo.typeParameters, typeArgs);
  std::vector<sun::TypePtr> paramTypes;
  for (const auto& [name, annot] : genericInfo.params) {
    paramTypes.push_back(substituteTypeParameters(typeAnnotationToType(annot)));
  }
  sun::TypePtr returnType = genericInfo.returnType
                                ? substituteTypeParameters(typeAnnotationToType(
                                      *genericInfo.returnType))
                                : sun::Types::Void();
  exitScope();
  bool canThrow = genericInfo.AST && genericInfo.AST->getProto().canThrow();
  return sun::Types::Function(returnType, paramTypes, canThrow);
}
