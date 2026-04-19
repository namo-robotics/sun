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
      return sun::Types::Lambda(newRet, std::move(newParams));
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
        // Need to re-instantiate the generic class with substituted type args
        std::string baseName = ct->getBaseGenericName();
        if (baseName.empty()) {
          // Might be an unresolved generic reference, extract base name from
          // class name
          baseName = ct->getName();
        }
        return instantiateGenericClass(baseName, newArgs);
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

  // Primitives, etc. don't need substitution
  return type;
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
    return sun::Types::Reference(referencedType);
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
    return sun::Types::Lambda(retType, std::move(paramTypes));
  }

  // Array types: array<T, N> or array<T, M, N> or array<T> (unsized)
  if (annot.isArray()) {
    if (!annot.elementType) {
      llvm::errs() << "Error: array type requires element type\n";
      return nullptr;
    }
    sun::TypePtr elemType = typeAnnotationToType(*annot.elementType);
    if (!elemType) {
      llvm::errs() << "Error: invalid array element type\n";
      return nullptr;
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
        llvm::errs() << "Error: Invalid type argument in generic type\n";
        return nullptr;
      }
      typeArgs.push_back(argType);
    }

    // Resolve the base name through using imports (Vec -> sun_Vec)
    std::string resolvedName = resolveNameWithUsings(annot.baseName).mangled();

    // Try to instantiate the generic class
    auto specializedClass = instantiateGenericClass(resolvedName, typeArgs);
    if (specializedClass) {
      return specializedClass;
    }

    // Try to instantiate as a generic interface (e.g., IIterator<i32>)
    auto specializedInterface =
        instantiateGenericInterface(resolvedName, typeArgs);
    if (specializedInterface) {
      return specializedInterface;
    }

    llvm::errs() << "Error: Failed to instantiate generic type '"
                 << annot.baseName << "'\n";
    return nullptr;
  }

  // Resolve the base name through using imports (Vec -> sun_Vec)
  std::string resolvedName = resolveNameWithUsings(annot.baseName).mangled();

  // Check for type aliases (lexically scoped)
  auto aliasType = findTypeAlias(resolvedName);
  if (aliasType) {
    return aliasType;
  }

  // Check for user-defined class types
  auto classType = lookupClass(resolvedName);
  if (classType) {
    return classType;
  }

  // Check for generic class definitions (used as type parameter in nested
  // generics)
  auto* genericInfo = lookupGenericClass(resolvedName);
  if (genericInfo) {
    // This is a reference to a generic class without type arguments
    // Return a type parameter type (this should really be an error in most
    // contexts)
    return sun::Types::TypeParameter(annot.baseName);
  }

  // Check for user-defined interface types
  auto interfaceType = lookupInterface(resolvedName);
  if (interfaceType) {
    return interfaceType;
  }

  // Check for user-defined enum types
  auto enumType = lookupEnum(resolvedName);
  if (enumType) {
    return enumType;
  }

  // Unknown type - could be a type parameter not yet bound
  // Create a TypeParameter type for it
  return sun::Types::TypeParameter(annot.baseName);
}
