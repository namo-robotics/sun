// semantic_analysis/type_conversion.cpp — Type annotation conversion and
// substitution

#include "error.h"
#include "semantic_analyzer.h"

// -------------------------------------------------------------------
// Type parameter substitution
// -------------------------------------------------------------------

// Substitute type parameters in a type using current bindings
sun::TypePtr SemanticAnalyzer::substituteTypeParameters(sun::TypePtr type) {
  if (!type) return nullptr;

  // If it's a type parameter, look up binding in scope stack
  if (type->isTypeParameter()) {
    auto* tp = dynamic_cast<sun::TypeParameterType*>(type.get());
    auto bound = findTypeParameter(tp->getName());
    return bound ? bound : type;  // Return bound type or original if not bound
  }

  // Recursively substitute in compound types
  if (type->isReference()) {
    auto* rt = dynamic_cast<sun::ReferenceType*>(type.get());
    auto newReferenced = substituteTypeParameters(rt->getReferencedType());
    if (newReferenced != rt->getReferencedType()) {
      return sun::Types::Reference(newReferenced, rt->isMutable());
    }
    return type;
  }

  if (type->isRawPointer()) {
    auto* pt = dynamic_cast<sun::RawPointerType*>(type.get());
    auto newPointee = substituteTypeParameters(pt->getPointeeType());
    if (newPointee != pt->getPointeeType()) {
      return sun::Types::RawPointer(newPointee);
    }
    return type;
  }

  if (type->isStaticPointer()) {
    auto* pt = dynamic_cast<sun::StaticPointerType*>(type.get());
    auto newPointee = substituteTypeParameters(pt->getPointeeType());
    if (newPointee != pt->getPointeeType()) {
      return sun::Types::StaticPointer(newPointee);
    }
    return type;
  }

  if (type->isFunction()) {
    auto* ft = dynamic_cast<sun::FunctionType*>(type.get());
    auto newRet = substituteTypeParameters(ft->getReturnType());
    std::vector<sun::TypePtr> newParams;
    bool changed = (newRet != ft->getReturnType());
    for (const auto& param : ft->getParamTypes()) {
      auto newParam = substituteTypeParameters(param);
      newParams.push_back(newParam);
      if (newParam != param) changed = true;
    }
    if (changed) {
      return sun::Types::Function(newRet, std::move(newParams));
    }
    return type;
  }

  if (type->isLambda()) {
    auto* lt = dynamic_cast<sun::LambdaType*>(type.get());
    auto newRet = substituteTypeParameters(lt->getReturnType());
    std::vector<sun::TypePtr> newParams;
    bool changed = (newRet != lt->getReturnType());
    for (const auto& param : lt->getParamTypes()) {
      auto newParam = substituteTypeParameters(param);
      newParams.push_back(newParam);
      if (newParam != param) changed = true;
    }
    if (changed) {
      return sun::Types::Lambda(newRet, std::move(newParams), lt->canThrow());
    }
    return type;
  }

  // Handle class types with type arguments (e.g., MatrixView<T> ->
  // MatrixView<i32>)
  if (type->isClass()) {
    auto* ct = dynamic_cast<sun::ClassType*>(type.get());
    const auto& typeArgs = ct->getTypeArguments();
    if (!typeArgs.empty()) {
      std::vector<sun::TypePtr> newArgs;
      bool changed = false;
      for (const auto& arg : typeArgs) {
        auto newArg = substituteTypeParameters(arg);
        newArgs.push_back(newArg);
        if (newArg != arg) changed = true;
      }
      if (changed) {
        // Re-instantiate the generic with substituted type args
        if (auto* info = lookupGenericClassOf(*ct)) {
          return instantiateGenericClass(*info, newArgs);
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
    auto* it = dynamic_cast<sun::InterfaceType*>(type.get());
    const auto& typeArgs = it->getTypeArguments();
    if (!typeArgs.empty()) {
      std::vector<sun::TypePtr> newArgs;
      bool changed = false;
      for (const auto& arg : typeArgs) {
        auto newArg = substituteTypeParameters(arg);
        newArgs.push_back(newArg);
        if (newArg != arg) changed = true;
      }
      if (changed) {
        // Need to re-instantiate the generic interface with substituted type
        // args
        std::string baseName = it->getBaseGenericName();
        if (baseName.empty()) {
          baseName = it->getName();
        }
        return instantiateGenericInterface(baseName, newArgs);
      }
    }
    return type;
  }

  // Handle specialized enums with type arguments (e.g., Option<T> ->
  // Option<i32>)
  if (type->isEnum()) {
    auto* et = dynamic_cast<sun::EnumType*>(type.get());
    const auto& typeArgs = et->getGenericArgs();
    if (!typeArgs.empty()) {
      std::vector<sun::TypePtr> newArgs;
      bool changed = false;
      for (const auto& arg : typeArgs) {
        auto newArg = substituteTypeParameters(arg);
        newArgs.push_back(newArg);
        if (newArg != arg) changed = true;
      }
      if (changed) {
        return instantiateGenericEnum(et->getGenericBase(), newArgs);
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

std::vector<sun::TypePtr> SemanticAnalyzer::resolveTypeArguments(
    const std::vector<std::unique_ptr<TypeAnnotation>>& typeAnnotations,
    const std::optional<Position>& location, const std::string& context) {
  std::vector<sun::TypePtr> typeArgs;
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

sun::TypePtr SemanticAnalyzer::typeAnnotationToType(
    const TypeAnnotation& annot) {
  // Raw pointer types: raw_ptr<T> non-owning pointer for C interop
  if (annot.isRawPointer()) {
    if (!annot.elementType) {
      return nullptr;
    }
    sun::TypePtr pointeeType = typeAnnotationToType(*annot.elementType);
    return sun::Types::RawPointer(pointeeType);
  }

  // Static pointer types: static_ptr<T> pointer to immortal static data
  if (annot.isStaticPointer()) {
    if (!annot.elementType) {
      return nullptr;
    }
    sun::TypePtr pointeeType = typeAnnotationToType(*annot.elementType);
    return sun::Types::StaticPointer(pointeeType);
  }

  // Reference types: ref(T) with implicit dereferencing
  if (annot.isReference()) {
    if (!annot.elementType) {
      return nullptr;
    }
    sun::TypePtr referencedType = typeAnnotationToType(*annot.elementType);
    if (!referencedType) return nullptr;
    return sun::Types::Reference(referencedType, /*isMutable=*/!annot.constRef);
  }

  // Function types: _() {} (named function, direct call)
  if (annot.isFunction()) {
    std::vector<sun::TypePtr> paramTypes;
    for (const auto& param : annot.paramTypes) {
      paramTypes.push_back(typeAnnotationToType(*param));
    }
    sun::TypePtr retType = annot.returnType
                               ? typeAnnotationToType(*annot.returnType)
                               : sun::Types::Void();
    return sun::Types::Function(retType, std::move(paramTypes));
  }

  // Lambda types: () {} (anonymous function, fat pointer call)
  if (annot.isLambda()) {
    std::vector<sun::TypePtr> paramTypes;
    for (const auto& param : annot.paramTypes) {
      paramTypes.push_back(typeAnnotationToType(*param));
    }
    sun::TypePtr retType = annot.returnType
                               ? typeAnnotationToType(*annot.returnType)
                               : sun::Types::Void();
    bool canThrow =
        annot.canError || (annot.returnType && annot.returnType->canError);
    return sun::Types::Lambda(retType, std::move(paramTypes), canThrow);
  }

  // Array types: array<T, N> or array<T, M, N> or array<T> (unsized)
  if (annot.isArray()) {
    if (!annot.elementType) {
      logAndThrowError("array type requires an element type", annot.span);
    }
    sun::TypePtr elemType = typeAnnotationToType(*annot.elementType);
    if (!elemType) {
      logAndThrowError("invalid array element type", annot.span);
    }
    // Empty dimensions means "unsized" - accepts any array<T, ...>
    return sun::Types::Array(elemType, annot.arrayDimensions);
  }

  // Try primitive types first
  sun::TypePtr primitiveType = sun::Types::fromString(annot.baseName);
  if (primitiveType) {
    return primitiveType;
  }

  // Check for type parameter binding (in generic context)
  auto typeParamBinding = findTypeParameter(annot.baseName);
  if (typeParamBinding) {
    return typeParamBinding;
  }

  // Check if this is a generic type usage like List<i32>
  if (annot.isGeneric()) {
    // Convert type arguments to TypePtrs
    std::vector<sun::TypePtr> typeArgs;
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
    sun::QualifiedName resolved = resolveNameWithUsings(annot.baseName);

    // Try to instantiate the generic class
    // Use the original dotted name for module-qualified lookups (e.g.,
    // "Test.Inner") so lookupGenericClass can find it via module path.
    // Fall back to resolved.baseName for using-imported names.
    std::string lookupName = annot.baseName.find('.') != std::string::npos
                                 ? annot.baseName
                                 : resolved.baseName;
    // Generic enum (e.g., Option<i32>) — checked first to avoid the noisy
    // unknown-class/interface fallthrough below
    if (currentScope->lookupGenericEnum(lookupName)) {
      auto specializedEnum = instantiateGenericEnum(lookupName, typeArgs);
      if (specializedEnum) {
        return specializedEnum;
      }
    }

    // Generic interface (e.g., IIterator<i32, Range>)
    if (lookupGenericInterface(lookupName)) {
      auto specializedInterface =
          instantiateGenericInterface(lookupName, typeArgs);
      if (specializedInterface) {
        return specializedInterface;
      }
    }

    // Guarded so the unknown-name case reports here, with a source location,
    // rather than from inside the instantiation helper.
    if (lookupGenericClass(lookupName)) {
      auto specializedClass = instantiateGenericClass(lookupName, typeArgs);
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
  sun::QualifiedName resolved = resolveNameWithUsings(annot.baseName);
  const std::string& lookupName =
      annot.baseName.find('.') != std::string::npos ? annot.baseName
                                                    : resolved.baseName;

  // Check for type aliases (lexically scoped)
  auto aliasType = findTypeAlias(lookupName);
  if (aliasType) {
    return aliasType;
  }

  // Check for user-defined class types
  auto classType = lookupClass(lookupName);
  if (classType) {
    return classType;
  }

  // Check for generic class definitions (used as type parameter in nested
  // generics)
  auto* genericInfo = lookupGenericClass(lookupName);
  if (genericInfo) {
    // This is a reference to a generic class without type arguments
    // Return a type parameter type (this should really be an error in most
    // contexts)
    return sun::Types::TypeParameter(annot.baseName);
  }

  // Check for user-defined interface types
  auto interfaceType = lookupInterface(lookupName);
  if (interfaceType) {
    return interfaceType;
  }

  // Check for user-defined enum types
  auto enumType = lookupEnum(lookupName);
  if (enumType) {
    return enumType;
  }

  // A dotted name spells a module path, so it is never an unbound type
  // parameter — reaching here means the module or the symbol is wrong. Say so
  // now; silently making a type parameter of it only surfaces later as a
  // mismatch against the type it was meant to name.
  if (annot.baseName.find('.') != std::string::npos) {
    logAndThrowError("Unknown type '" + annot.baseName +
                         "'. No class, interface or enum by that name is "
                         "visible here — check the spelling, and that the "
                         "module declaring it is imported in this scope.",
                     annot.span);
  }

  // Unknown type - could be a type parameter not yet bound
  // Create a TypeParameter type for it
  return sun::Types::TypeParameter(annot.baseName);
}

// -------------------------------------------------------------------
// Const view
// -------------------------------------------------------------------

// The const view of a type: every `ref` in it becomes `const ref`. A payload
// enum such as Option<ref T> is re-instantiated as Option<const ref T>; both
// lower to the same layout, so a value crosses between them through memory
// without codegen help. Classes are not viewed (a class holding a borrow of
// its receiver is not a pattern the stdlib uses).
sun::TypePtr SemanticAnalyzer::createConstView(sun::TypePtr type) {
  if (!type) return type;
  if (type->isReference()) {
    auto* ref = static_cast<const sun::ReferenceType*>(type.get());
    if (!ref->isMutable()) return type;
    return sun::Types::Reference(ref->getReferencedType(), /*isMutable=*/false);
  }
  if (type->isEnum()) {
    auto* enumType = static_cast<const sun::EnumType*>(type.get());
    if (!enumType->isGenericSpecialization()) return type;
    std::vector<sun::TypePtr> args;
    bool changed = false;
    for (const auto& arg : enumType->getGenericArgs()) {
      sun::TypePtr viewed = createConstView(arg);
      changed = changed || viewed != arg;
      args.push_back(viewed);
    }
    if (!changed) return type;
    if (auto viewed = instantiateGenericEnum(enumType->getGenericBase(), args)) {
      return viewed;
    }
  }
  return type;
}
