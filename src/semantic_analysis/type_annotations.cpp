// type_annotations.cpp — Resolving a written type annotation to a type, and
// the const view of a type (see type_inferer.h)

#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_inferer.h"
#include "semantic_analysis/type_traits.h"
#include "support/error.h"

using sun::semantic_analysis::DeclarationKind;
using sun::semantic_analysis::InterfaceType;
using sun::semantic_analysis::LambdaType;
using sun::semantic_analysis::ReferenceType;
using sun::semantic_analysis::TypePtr;
using sun::semantic_analysis::Types;

using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

using sun::semantic_analysis::unwrapRef;

/** Keeps the implementation helpers in this file private to this translation unit. */
namespace {

/**
 * What `_return_type_of<F>` stands for once F is known. A lambda or function
 * answers with what it returns; a parameter that is still standing for itself
 * keeps the question open, carried as the parameter plus the projection.
 */
TypePtr returnTypeOf(const TypePtr& target,
                     std::optional<sun::support::Position> at) {
  if (!target) return nullptr;
  if (auto* lambda = sun::codegen::support::tryGetType<LambdaType>(target)) {
    return lambda->getReturnType();
  }
  if (auto* func = sun::codegen::support::tryGetType<
          sun::semantic_analysis::FunctionType>(target)) {
    return func->getReturnType();
  }
  if (auto* param = sun::codegen::support::tryGetType<
          sun::semantic_analysis::TypeParameterType>(target)) {
    return param->project(sun::semantic_analysis::TypeProjection::ReturnType);
  }
  logAndThrowError("_return_type_of<" + target->toDisplayString() +
                       "> requires a lambda or function type",
                   at);
  return nullptr;
}

}  // namespace

std::shared_ptr<InterfaceType> TypeInferer::resolveConstraintInterface(
    const sun::ast::TypeConstraint& constraint) {
  auto type = typeAnnotationToType(constraint.toAnnotation());
  if (!type || !type->isInterface()) {
    logAndThrowError(
        "constraint '" + constraint.toString() + "' must name an interface",
        constraint.span);
  }
  return std::static_pointer_cast<InterfaceType>(type);
}

TypePtr TypeInferer::substituteTypeParameters(TypePtr type) {
  if (!type) return nullptr;

  // If it's a type parameter, look up binding in scope stack. A projected one
  // asks after its base and then applies the projection to whatever that
  // turned out to be.
  if (type->isTypeParameter()) {
    auto* tp =
        dynamic_cast<sun::semantic_analysis::TypeParameterType*>(type.get());
    auto bound = ctx_.findTypeParameter(tp->getProjectionBase());
    if (!bound) return type;
    return tp->getProjection() ==
                   sun::semantic_analysis::TypeProjection::ReturnType
               ? returnTypeOf(bound, std::nullopt)
               : bound;
  }

  // Recursively substitute in compound types
  if (type->isReference()) {
    auto* rt = dynamic_cast<ReferenceType*>(type.get());
    auto newReferenced = substituteTypeParameters(rt->getReferencedType());
    if (newReferenced != rt->getReferencedType()) {
      return Types::Reference(newReferenced, rt->isMutable());
    }
    return type;
  }

  if (type->isRawPointer()) {
    auto* pt =
        dynamic_cast<sun::semantic_analysis::RawPointerType*>(type.get());
    auto newPointee = substituteTypeParameters(pt->getPointeeType());
    if (newPointee != pt->getPointeeType()) {
      return Types::RawPointer(newPointee);
    }
    return type;
  }

  if (type->isStaticPointer()) {
    auto* pt =
        dynamic_cast<sun::semantic_analysis::StaticPointerType*>(type.get());
    auto newPointee = substituteTypeParameters(pt->getPointeeType());
    if (newPointee != pt->getPointeeType()) {
      return Types::StaticPointer(newPointee);
    }
    return type;
  }

  if (type->isFunction()) {
    auto* ft = dynamic_cast<sun::semantic_analysis::FunctionType*>(type.get());
    auto newRet = substituteTypeParameters(ft->getReturnType());
    std::vector<TypePtr> newParams;
    bool changed = (newRet != ft->getReturnType());
    for (const auto& param : ft->getParamTypes()) {
      auto newParam = substituteTypeParameters(param);
      newParams.push_back(newParam);
      if (newParam != param) changed = true;
    }
    if (changed) {
      return Types::Function(newRet, std::move(newParams), ft->canThrow(),
                             ft->requiresUnsafe());
    }
    return type;
  }

  if (type->isLambda()) {
    auto* lt = dynamic_cast<LambdaType*>(type.get());
    auto newRet = substituteTypeParameters(lt->getReturnType());
    std::vector<TypePtr> newParams;
    bool changed = (newRet != lt->getReturnType());
    for (const auto& param : lt->getParamTypes()) {
      auto newParam = substituteTypeParameters(param);
      newParams.push_back(newParam);
      if (newParam != param) changed = true;
    }
    if (changed) {
      auto substituted = Types::Lambda(newRet, std::move(newParams),
                                       lt->canThrow(), lt->requiresUnsafe());
      // The <'_> marker is part of the type's identity and must survive
      // substitution, or spawn<F>'s specializations would lose it; the
      // lifetime metadata rides along with it
      static_cast<LambdaType*>(substituted.get())
          ->setHasRefCaptures(lt->hasRefCaptures());
      static_cast<LambdaType*>(substituted.get())
          ->setLifetimeName(lt->getLifetimeName());
      return substituted;
    }
    return type;
  }

  // Handle class types with type arguments (e.g., MatrixView<T> ->
  // MatrixView<i32>)
  if (type->isClass()) {
    auto* ct = dynamic_cast<sun::semantic_analysis::ClassType*>(type.get());
    const auto& typeArgs = ct->getTypeArguments();
    if (!typeArgs.empty()) {
      std::vector<TypePtr> newArgs;
      bool changed = false;
      for (const auto& arg : typeArgs) {
        auto newArg = substituteTypeParameters(arg);
        newArgs.push_back(newArg);
        if (newArg != arg) changed = true;
      }
      if (changed) {
        // Re-instantiate the generic with substituted type args
        if (auto* info = generics_.lookupGenericClassOf(*ct)) {
          return generics_.instantiateGenericClass(*info, newArgs);
        }
        logAndThrowError("Cannot resolve generic class for '" +
                         ct->getDisplayName() + "'");
        return type;
      }
    }
    return type;
  }

  // Handle interface types with type arguments (e.g., IIterator<T> ->
  // IIterator<i32>)
  if (type->isInterface()) {
    auto* it = dynamic_cast<InterfaceType*>(type.get());
    const auto& typeArgs = it->getTypeArguments();
    if (!typeArgs.empty()) {
      std::vector<TypePtr> newArgs;
      bool changed = false;
      for (const auto& arg : typeArgs) {
        auto newArg = substituteTypeParameters(arg);
        newArgs.push_back(newArg);
        if (newArg != arg) changed = true;
      }
      if (changed) {
        auto* info = ctx_.lookupGenericInterface(
            it->sourceDeclaration(ctx_.types()->declarations));
        if (!info)
          logAndThrowError("Selected generic interface is not registered");
        return generics_.instantiateGenericInterface(*info, newArgs);
      }
    }
    return type;
  }

  // Handle specialized enums with type arguments (e.g., Option<T> ->
  // Option<i32>)
  if (type->isEnum()) {
    auto* et = dynamic_cast<sun::semantic_analysis::EnumType*>(type.get());
    const auto& typeArgs = et->getGenericArgs();
    if (!typeArgs.empty()) {
      std::vector<TypePtr> newArgs;
      bool changed = false;
      for (const auto& arg : typeArgs) {
        auto newArg = substituteTypeParameters(arg);
        newArgs.push_back(newArg);
        if (newArg != arg) changed = true;
      }
      if (changed) {
        auto* info = ctx_.lookupGenericEnum(
            et->sourceDeclaration(ctx_.types()->declarations));
        if (!info) logAndThrowError("Selected generic enum is not registered");
        return generics_.instantiateGenericEnum(*info, newArgs);
      }
    }
    return type;
  }

  // Primitives, etc. don't need substitution
  return type;
}

// -------------------------------------------------------------------
// Type argument resolution helper
// -------------------------------------------------------------------

std::vector<TypePtr> TypeInferer::resolveTypeArguments(
    const std::vector<std::unique_ptr<sun::ast::TypeAnnotation>>&
        typeAnnotations,
    const std::optional<sun::support::Position>& location,
    const std::string& context) {
  std::vector<TypePtr> typeArgs;
  for (const auto& typeArg : typeAnnotations) {
    auto argType = typeAnnotationToType(*typeArg);
    if (!argType) {
      logAndThrowError("Invalid type argument in " + context, location);
    }
    typeArgs.push_back(argType);
  }
  return typeArgs;
}

// -------------------------------------------------------------------
// Type annotation to type conversion
// -------------------------------------------------------------------

TypePtr TypeInferer::typeAnnotationToType(
    const sun::ast::TypeAnnotation& annot) {
  if (annot.declarationKey) {
    auto id = ctx_.requireDeclaration(*annot.declarationKey, "", std::nullopt,
                                      annot.baseName);
    std::vector<TypePtr> arguments;
    for (const auto& argument : annot.typeArguments)
      arguments.push_back(typeAnnotationToType(*argument));
    auto kind = ctx_.types()->declarations.get(id).kind;
    if (!arguments.empty()) {
      if (kind == DeclarationKind::Class) {
        auto* info = ctx_.lookupGenericClass(id);
        if (info) return generics_.instantiateGenericClass(*info, arguments);
      } else if (kind == DeclarationKind::Interface) {
        auto* info = ctx_.lookupGenericInterface(id);
        if (info)
          return generics_.instantiateGenericInterface(*info, arguments);
      } else if (kind == DeclarationKind::Enum) {
        auto* info = ctx_.lookupGenericEnum(id);
        if (info) return generics_.instantiateGenericEnum(*info, arguments);
      }
      logAndThrowError("Imported generic type has no registered template",
                       annot.span);
    }
    if (kind == DeclarationKind::Class) return ctx_.types()->getClass(id);
    if (kind == DeclarationKind::Interface)
      return ctx_.types()->getInterface(id);
    return ctx_.types()->getEnum(id);
  }
  // Raw pointer types: raw_ptr<T> non-owning pointer for C interop
  if (annot.isRawPointer()) {
    if (!annot.elementType) {
      return nullptr;
    }
    TypePtr pointeeType = typeAnnotationToType(*annot.elementType);
    return Types::RawPointer(pointeeType);
  }

  // Static pointer types: static_ptr<T> pointer to immortal static data
  if (annot.isStaticPointer()) {
    if (!annot.elementType) {
      return nullptr;
    }
    TypePtr pointeeType = typeAnnotationToType(*annot.elementType);
    return Types::StaticPointer(pointeeType);
  }

  // Reference types: ref(T) with implicit dereferencing
  if (annot.isReference()) {
    if (!annot.elementType) {
      return nullptr;
    }
    // `ref array<T>` is the one place an unsized array may be written: a
    // view of some sized array with its rank erased
    TypePtr referencedType;
    if (annot.elementType->isArray() &&
        annot.elementType->arrayDimensions.empty()) {
      if (!annot.elementType->elementType) {
        logAndThrowError("array type requires an element type", annot.span);
      }
      TypePtr elemType = typeAnnotationToType(*annot.elementType->elementType);
      if (!elemType) {
        logAndThrowError("invalid array element type", annot.span);
      }
      referencedType = Types::Array(elemType, {});
    } else {
      referencedType = typeAnnotationToType(*annot.elementType);
    }
    if (!referencedType) return nullptr;
    auto refType =
        Types::Reference(referencedType, /*isMutable=*/!annot.constRef);
    // A named lifetime on the position ('ref 'a Bus') rides along as
    // metadata for the borrow checker; it is not part of the type. So do
    // the class application's lifetime arguments ('ref Bus<'this>'), which
    // must match the class's declared lifetimes one for one.
    static_cast<ReferenceType*>(refType.get())
        ->setLifetimeName(annot.lifetimeName);
    if (!annot.elementType->lifetimeArguments.empty()) {
      auto* referentClass =
          sun::codegen::support::tryGetType<sun::semantic_analysis::ClassType>(
              referencedType);
      size_t declared =
          referentClass ? referentClass->getLifetimeParams().size() : 0;
      if (annot.elementType->lifetimeArguments.size() != declared) {
        logAndThrowError(
            "'" + annot.elementType->baseName + "' declares " +
                std::to_string(declared) + " lifetime parameter(s), but " +
                std::to_string(annot.elementType->lifetimeArguments.size()) +
                " are applied here",
            annot.span);
      }
      static_cast<ReferenceType*>(refType.get())
          ->setClassLifetimeArgs(annot.elementType->lifetimeArguments);
    }
    return refType;
  }

  // Function-pointer types: function (Types) ReturnType
  if (annot.isFunction()) {
    if (annot.returnType && annot.returnType->refEnv &&
        (annot.returnType->lifetimeName.empty() ||
         annot.returnType->lifetimeName == "_")) {
      logAndThrowError(
          "an anonymous <'_> lambda type cannot be a return type - its "
          "captured "
          "environment lives in a stack frame that dies when the function "
          "returns",
          annot.span);
    }
    std::vector<TypePtr> paramTypes;
    for (const auto& param : annot.paramTypes) {
      paramTypes.push_back(typeAnnotationToType(*param));
    }
    TypePtr retType = annot.returnType ? typeAnnotationToType(*annot.returnType)
                                       : Types::Void();
    return Types::Function(retType, std::move(paramTypes), annot.canError,
                           annot.requiresUnsafe);
  }

  // Lambda types: () {} (anonymous function, fat pointer call)
  if (annot.isLambda()) {
    if (annot.returnType && annot.returnType->refEnv &&
        (annot.returnType->lifetimeName.empty() ||
         annot.returnType->lifetimeName == "_")) {
      logAndThrowError(
          "an anonymous <'_> lambda type cannot be a return type - its "
          "captured "
          "environment lives in a stack frame that dies when the function "
          "returns",
          annot.span);
    }
    std::vector<TypePtr> paramTypes;
    for (const auto& param : annot.paramTypes) {
      paramTypes.push_back(typeAnnotationToType(*param));
    }
    TypePtr retType = annot.returnType ? typeAnnotationToType(*annot.returnType)
                                       : Types::Void();
    bool canThrow =
        annot.canError || (annot.returnType && annot.returnType->canError);
    auto lambdaType = Types::Lambda(retType, std::move(paramTypes), canThrow,
                                    annot.requiresUnsafe);
    // `<'_>(…) => …` admits lambdas whose captured environment lives in a
    // stack frame; a plain annotation admits only environment-free lambdas.
    // A named lifetime ('<'a>') rides along as borrow-checker metadata.
    static_cast<LambdaType*>(lambdaType.get())->setHasRefCaptures(annot.refEnv);
    static_cast<LambdaType*>(lambdaType.get())
        ->setLifetimeName(annot.lifetimeName);
    return lambdaType;
  }

  // Array types: array<T, N> or array<T, M, N>. An unsized array<T> is a
  // view of storage owned elsewhere and may only be written behind `ref`
  // (handled by the reference case above).
  if (annot.isArray()) {
    if (!annot.elementType) {
      logAndThrowError("array type requires an element type", annot.span);
    }
    if (annot.arrayDimensions.empty()) {
      logAndThrowError(
          "an unsized array<T> is a view of a sized array and may only be "
          "used behind ref: write 'ref array<T>' or 'const ref array<T>', or "
          "give it a size: array<T, N>",
          annot.span);
    }
    TypePtr elemType = typeAnnotationToType(*annot.elementType);
    if (!elemType) {
      logAndThrowError("invalid array element type", annot.span);
    }
    return Types::Array(elemType, annot.arrayDimensions);
  }

  // Try primitive types first
  TypePtr primitiveType = Types::fromString(annot.baseName);
  if (primitiveType) {
    return primitiveType;
  }

  // Builtin traits are symbolic targets of _is and generic constraints.
  if (sun::semantic_analysis::isTypeTrait(annot.baseName)) {
    return Types::TypeParameter(annot.baseName);
  }

  // Check for type parameter binding (in generic context)
  auto typeParamBinding = ctx_.findTypeParameter(annot.baseName);
  if (typeParamBinding) {
    return typeParamBinding;
  }

  // _return_type_of<F>: what F returns. A type computed from another type
  // rather than named outright, so it is resolved here rather than looked up.
  if (annot.baseName == "_return_type_of") {
    if (annot.typeArguments.size() != 1) {
      logAndThrowError("_return_type_of takes exactly one type argument",
                       annot.span);
    }
    return returnTypeOf(typeAnnotationToType(*annot.typeArguments[0]),
                        annot.span);
  }

  // Check if this is a generic type usage like List<i32>
  if (annot.isGeneric()) {
    // Convert type arguments to TypePtrs
    std::vector<TypePtr> typeArgs;
    for (const auto& typeArg : annot.typeArguments) {
      auto argType = typeAnnotationToType(*typeArg);
      if (!argType) {
        logAndThrowError(
            "invalid type argument in generic type '" + annot.baseName + "'",
            annot.span);
      }
      typeArgs.push_back(argType);
    }

    // Resolve the base name through using imports
    sun::semantic_analysis::QualifiedName resolved =
        ctx_.resolveNameWithUsings(annot.baseName);

    // Try to instantiate the generic class
    // Use the original dotted name for module-qualified lookups (e.g.,
    // "Test.Inner") so lookupGenericClass can find it via module path.
    // Fall back to resolved.baseName for using-imported names.
    std::string lookupName = annot.baseName.find('.') != std::string::npos
                                 ? annot.baseName
                                 : resolved.baseName;
    // Generic enum (e.g., Option<i32>) — checked first to avoid the noisy
    // unknown-class/interface fallthrough below
    if (auto* info = ctx_.lookupGenericEnum(lookupName)) {
      auto specializedEnum = generics_.instantiateGenericEnum(*info, typeArgs);
      if (specializedEnum) {
        return specializedEnum;
      }
    }

    // Generic interface (e.g., IIterator<i32, Range>)
    if (auto* info = ctx_.lookupGenericInterface(lookupName)) {
      auto specializedInterface =
          generics_.instantiateGenericInterface(*info, typeArgs);
      if (specializedInterface) {
        return specializedInterface;
      }
    }

    // Guarded so the unknown-name case reports here, with a source location,
    // rather than from inside the instantiation helper.
    if (ctx_.lookupGenericClass(lookupName)) {
      auto specializedClass =
          generics_.instantiateGenericClass(lookupName, typeArgs);
      if (specializedClass) {
        return specializedClass;
      }
    }

    logAndThrowError("Unknown generic type '" + annot.baseName +
                         "'. No generic class, interface or enum by that name "
                         "is visible here — check the spelling, and that the "
                         "module declaring it is imported in this scope.",
                     annot.span);
  }

  // Resolve the base name through using imports. A dotted name keeps its
  // module path, so the lookups below can find the symbol inside that module
  // (the generic branch above does the same).
  sun::semantic_analysis::QualifiedName resolved =
      ctx_.resolveNameWithUsings(annot.baseName);
  const std::string& lookupName = annot.baseName.find('.') != std::string::npos
                                      ? annot.baseName
                                      : resolved.baseName;

  // Check for type aliases (lexically scoped)
  auto aliasType = ctx_.findTypeAlias(lookupName);
  if (aliasType) {
    return aliasType;
  }

  // Check for user-defined class types
  auto classType = ctx_.lookupClass(lookupName);
  if (classType) {
    return classType;
  }

  // Check for generic class definitions (used as type parameter in nested
  // generics)
  auto* genericInfo = ctx_.lookupGenericClass(lookupName);
  if (genericInfo) {
    // This is a reference to a generic class without type arguments
    // Return a type parameter type (this should really be an error in most
    // contexts)
    return Types::TypeParameter(annot.baseName, {},
                                genericInfo->AST->getDeclarationId(),
                                ctx_.types()->declarations.session());
  }

  // Check for user-defined interface types
  auto interfaceType = ctx_.lookupInterface(lookupName);
  if (interfaceType) {
    return interfaceType;
  }

  // Check for user-defined enum types
  auto enumType = ctx_.lookupEnum(lookupName);
  if (enumType) {
    return enumType;
  }

  logAndThrowError("Unknown type '" + annot.baseName +
                       "'. No class, interface or enum by that name is "
                       "visible here — check the spelling, and that the "
                       "module declaring it is imported in this scope.",
                   annot.span);
}

// -------------------------------------------------------------------
// Const view
// -------------------------------------------------------------------

// The const view of a type: every `ref` in it becomes `const ref`. A payload
// enum such as Option<ref T> is re-instantiated as Option<const ref T>; both
// lower to the same layout, so a value crosses between them through memory
// without codegen help. Classes are not viewed (a class holding a borrow of
// its receiver is not a pattern the stdlib uses).
TypePtr TypeInferer::createConstView(TypePtr type) {
  if (!type) return type;
  if (type->isReference()) {
    auto* ref = static_cast<const ReferenceType*>(type.get());
    if (!ref->isMutable()) return type;
    return Types::Reference(ref->getReferencedType(), /*isMutable=*/false);
  }
  if (type->isEnum()) {
    auto* enumType =
        static_cast<const sun::semantic_analysis::EnumType*>(type.get());
    if (!enumType->isGenericSpecialization()) return type;
    std::vector<TypePtr> args;
    bool changed = false;
    for (const auto& arg : enumType->getGenericArgs()) {
      TypePtr viewed = createConstView(arg);
      changed = changed || viewed != arg;
      args.push_back(viewed);
    }
    if (!changed) return type;
    auto* info = ctx_.lookupGenericEnum(
        enumType->sourceDeclaration(ctx_.types()->declarations));
    if (!info) logAndThrowError("Selected generic enum is not registered");
    return generics_.instantiateGenericEnum(*info, args);
  }
  return type;
}

}  // namespace sun::semantic_analysis
