/** Resolves expression declarations and records identities for code generation.
 */
#include "ast/control_flow.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_analysis/type_traits.h"
#include "support/error.h"

using sun::ast::ASTNodeType;
using sun::ast::MemberAccessAST;
using sun::ast::VariableReferenceAST;
using sun::support::logAndThrowError;
/** Resolves names, members, and the semantic facts needed by inference. */
namespace sun::semantic_analysis {
using sun::types::ClassMethod;
using sun::types::ClassType;
using sun::types::EnumType;
using sun::types::InterfaceField;
using sun::types::InterfaceMethod;
using sun::types::StaticPointerType;
using sun::types::TypePtr;
using sun::types::Types;
using sun::types::unwrapRef;

TypePtr SemanticAnalyzer::resolveVariableReferenceType(
    const VariableReferenceAST& varRef) {
  const std::string& name = varRef.getName();

  // Look up as variable
  VariableInfo* info = ctx_.currentScope().lookupVariable(name);
  if (info) {
    varRef.setTargetDeclarationId(info->declarationId);
    // Substitute type parameters to get concrete type (e.g., T -> Box)
    TypePtr originalType = resolver_.substituteTypeParameters(info->type);

    // Check for type narrowing from _is<T> guards
    // getNarrowedType returns the more specific type (class over interface)
    TypePtr narrowedType = ctx_.getNarrowedType(name, originalType);
    if (narrowedType) {
      return narrowedType;
    }

    return originalType;
  }

  // Check if it's an enum type name (for EnumName.Variant access)
  auto enumType = ctx_.lookupEnum(name);
  if (enumType) {
    return enumType;
  }

  // Resolve the name through using imports (e.g., Vec -> sun_Vec)
  auto resolved = ctx_.resolveNameWithUsings(name);

  // Check if it's a named function
  auto funcs = ctx_.getAllFunctions(resolved.baseName);
  if (funcs.size() == 1) {
    varRef.setTargetDeclarationId(funcs[0].declarationId);
    return Types::Function(funcs[0].returnType, funcs[0].paramTypes,
                           funcs[0].canThrow);
  }
  if (funcs.size() > 1) {
    logAndThrowError("Cannot reference overloaded function '" + name +
                         "' as a value; call it with arguments instead",
                     varRef.getLocation());
  }

  // Check if it's a module name (for mod_x.mod_y.var access)
  if (ctx_.isModuleName(name)) {
    std::string fullPath = ctx_.getFullModulePath(name);
    if (auto* modScope = ctx_.lookupModuleScope(fullPath))
      ctx_.requireModuleAccessible(*modScope, varRef.getLocation());
    return Types::Module(fullPath);
  }

  // Unknown variable - error in strongly typed language
  logAndThrowError("Unknown variable: '" + name + "'", varRef.getLocation());
}

TypePtr SemanticAnalyzer::resolveModuleMemberType(
    const MemberAccessAST& memberAccess, const TypePtr& objectType,
    const std::string& memberName) {
  // Module member access: mod_x.mod_y or mod_x.varName
  auto* moduleType = static_cast<sun::types::ModuleType*>(objectType.get());
  std::string modPath = moduleType->getModulePath();

  std::string nestedModPath = modPath + "." + memberName;
  if (auto* nested = ctx_.lookupModuleScope(nestedModPath)) {
    ctx_.requireModuleAccessible(*nested, memberAccess.getLocation());
    return Types::Module(QualifiedName::joinPath(nested->scopePath));
  }

  // Use unified symbol lookup to find the member in this module
  SymbolMatch match = ctx_.findSymbolInModule(modPath, memberName);
  if (match) {
    // Name the access after the declaration it resolved to — e.g.
    // "$d9b854ae$_std_make_heap_allocator" for std.make_heap_allocator.
    // Each declaration was given its name when it was declared, and
    // codegen emits it under that name; rebuilding one from the module
    // path here would drop a function's overload suffix and name a symbol
    // that was never emitted.
    QualifiedName resolvedName;
    switch (match.kind) {
      case SymbolKind::Function:
        if (match.functionInfo)
          resolvedName = match.functionInfo->qualifiedName;
        break;
      case SymbolKind::Variable:
        if (match.variableInfo)
          resolvedName = match.variableInfo->qualifiedName;
        break;
      case SymbolKind::Class:
        if (match.classType) resolvedName = match.classType->getQualifiedName();
        break;
      case SymbolKind::GenericClass:
        if (match.genericClassInfo)
          resolvedName = match.genericClassInfo->qualifiedName;
        break;
      case SymbolKind::Interface:
        if (match.interfaceType)
          resolvedName = match.interfaceType->getQualifiedName();
        break;
      case SymbolKind::Enum:
        if (match.enumType) resolvedName = match.enumType->getQualifiedName();
        break;
      default:
        break;
    }
    // A kind that carries no name of its own (a nested module, say) still
    // needs one a lookup can use.
    if (resolvedName.empty()) {
      resolvedName = QualifiedName(
          sun::semantic_analysis::splitModulePath(match.modulePath),
          memberName);
    }
    memberAccess.setQualifiedName(resolvedName);

    switch (match.kind) {
      case SymbolKind::Class:
        return match.classType;
      case SymbolKind::GenericClass: {
        // If type arguments are provided, instantiate the generic class
        if (memberAccess.hasTypeArguments() && match.genericClassInfo) {
          auto typeArgs = resolver_.resolveTypeArguments(
              memberAccess.getTypeArguments(), memberAccess.getLocation(),
              "generic class instantiation");
          // Store resolved type args on the AST for codegen
          memberAccess.setResolvedTypeArgs(typeArgs);
          // Instantiate the generic class with module-qualified name
          // (modPath.memberName so lookupGenericClass can find it)
          std::string qualifiedName = modPath + "." + memberName;
          auto specializedClass =
              generics_.instantiateGenericClass(qualifiedName, typeArgs);
          if (specializedClass) {
            return specializedClass;
          }
          logAndThrowError(
              "Failed to instantiate generic class '" + memberName + "'",
              memberAccess.getLocation());
        }
        // No type arguments - return void as placeholder
        return Types::Void();
      }
      case SymbolKind::Interface:
        return match.interfaceType;
      case SymbolKind::GenericInterface: {
        // If type arguments are provided, instantiate the generic interface
        if (memberAccess.hasTypeArguments() && match.genericInterfaceInfo) {
          auto typeArgs = resolver_.resolveTypeArguments(
              memberAccess.getTypeArguments(), memberAccess.getLocation(),
              "generic interface instantiation");
          // Store resolved type args on the AST for codegen
          memberAccess.setResolvedTypeArgs(typeArgs);
          // Instantiate the generic interface with module-qualified name
          std::string qualifiedName = modPath + "." + memberName;
          auto specializedInterface =
              generics_.instantiateGenericInterface(qualifiedName, typeArgs);
          if (specializedInterface) {
            return specializedInterface;
          }
          logAndThrowError(
              "Failed to instantiate generic interface '" + memberName + "'",
              memberAccess.getLocation());
        }
        // No type arguments - return void as placeholder
        return Types::Void();
      }
      case SymbolKind::Enum:
        return match.enumType;
      case SymbolKind::Function:
        memberAccess.setTargetDeclarationId(match.functionInfo->declarationId);
        return Types::Function(match.functionInfo->returnType,
                               match.functionInfo->paramTypes,
                               match.functionInfo->canThrow);
      case SymbolKind::GenericFunction: {
        // m.f<i32>(...): instantiate here, and point the call site at the
        // specialization rather than at the template's name. A call site
        // that inferred its type arguments from the arguments (see
        // CallAnalyzer::resolveModuleQualifiedGenericCall) has already
        // recorded them; only a bare reference to the template has none.
        if (!memberAccess.hasTypeArguments() &&
            !memberAccess.hasResolvedTypeArgs()) {
          logAndThrowError(
              "Generic function '" + memberName + "' in module '" +
                  sun::semantic_analysis::displayModulePath(modPath) +
                  "' needs type arguments here; they are only inferred at a "
                  "call, e.g. " +
                  memberName + "<i32>(...)",
              memberAccess.getLocation());
        }
        if (!match.genericFunctionInfo) {
          logAndThrowError(
              "Generic function '" + memberName + "' in module '" +
                  sun::semantic_analysis::displayModulePath(modPath) +
                  "' has no definition",
              memberAccess.getLocation());
        }
        std::vector<TypePtr> typeArgs =
            memberAccess.hasResolvedTypeArgs()
                ? memberAccess.getResolvedTypeArgs()
                : resolver_.resolveTypeArguments(
                      memberAccess.getTypeArguments(),
                      memberAccess.getLocation(),
                      "generic function instantiation");
        memberAccess.setResolvedTypeArgs(typeArgs);
        SpecializedFunctionInfo specialized =
            generics_.requireGenericSpecialization(*match.genericFunctionInfo,
                                                   typeArgs, memberName,
                                                   memberAccess.getLocation());
        memberAccess.setQualifiedName(specialized.qualifiedName);
        memberAccess.setTargetDeclarationId(
            specialized.asFunctionInfo().declarationId);
        return specialized.functionType();
      }
      case SymbolKind::Variable:
        memberAccess.setTargetDeclarationId(match.variableInfo->declarationId);
        return match.variableInfo->type;
      default:
        break;
    }
  }

  logAndThrowError("Unknown member '" + memberName + "' in module '" +
                       sun::semantic_analysis::displayModulePath(modPath) + "'",
                   memberAccess.getLocation());
}

TypePtr SemanticAnalyzer::resolveClassMemberType(
    const MemberAccessAST& memberAccess, const TypePtr& objectType,
    const std::string& memberName) {
  const auto* classType = static_cast<const ClassType*>(objectType.get());

  // Check for field
  const sun::types::ClassField* field =
      ctx_.accessibleField(*classType, memberName, memberAccess.getLocation());
  if (field) {
    memberAccess.setTargetDeclarationId(field->declarationId);
    return field->type;
  }

  // Check for method
  const ClassMethod* method =
      ctx_.accessibleMethod(*classType, memberName, memberAccess.getLocation());
  if (method) {
    if (!method->isGeneric())
      memberAccess.setTargetDeclarationId(method->declarationId);
    TypePtr returnType = method->returnType;

    // Generic method calls: the type arguments written at the call,
    // completed by the ones the call site inferred from its arguments
    if (method->isGeneric()) {
      const auto& typeParams = method->typeParameters;

      std::vector<TypePtr> typeArgPtrs;
      for (const auto& typeArg : memberAccess.getTypeArguments()) {
        auto argType = resolver_.typeAnnotationToType(*typeArg);
        if (argType) {
          typeArgPtrs.push_back(argType);
        }
      }
      if (memberAccess.hasResolvedTypeArgs() &&
          memberAccess.getResolvedTypeArgs().size() > typeArgPtrs.size()) {
        typeArgPtrs = memberAccess.getResolvedTypeArgs();
      }

      // Store resolved type args on the AST for codegen
      memberAccess.setResolvedTypeArgs(typeArgPtrs);

      if (!typeArgPtrs.empty() && typeArgPtrs.size() == typeParams.size()) {
        // Instantiate the generic method - this creates and stores the
        // specialized FunctionAST on the generic method for codegen access
        auto mutableClassType = std::static_pointer_cast<ClassType>(objectType);
        // Retain the selected specialization independently of its symbol.
        if (auto specialized = generics_.instantiateGenericMethod(
                mutableClassType, memberName, typeArgPtrs)) {
          memberAccess.setTargetDeclarationId(specialized->getDeclarationId());
        }

        ctx_.enterTypeParamScope(typeParams, typeArgPtrs);
        returnType = resolver_.substituteTypeParameters(returnType);
        std::vector<TypePtr> substitutedParams;
        for (const auto& pt : method->paramTypes) {
          substitutedParams.push_back(resolver_.substituteTypeParameters(pt));
        }
        ctx_.exitScope();
        return Types::Function(returnType, substitutedParams, method->canThrow,
                               method->isUnsafe);
      }
    }

    return Types::Function(method->returnType, method->paramTypes,
                           method->canThrow, method->isUnsafe);
  }

  logAndThrowError("Unknown member '" + memberName + "' on class '" +
                       classType->getDisplayName() + "'",
                   memberAccess.getLocation());
}

TypePtr SemanticAnalyzer::resolveInterfaceMemberType(
    const MemberAccessAST& memberAccess, const TypePtr& objectType,
    const std::string& memberName) {
  const auto* ifaceType =
      static_cast<const sun::types::InterfaceType*>(objectType.get());

  // Check for field
  const InterfaceField* field =
      ctx_.accessibleField(*ifaceType, memberName, memberAccess.getLocation());
  if (field) {
    memberAccess.setTargetDeclarationId(field->declarationId);
    return field->type;
  }

  // Check for method
  const InterfaceMethod* method =
      ctx_.accessibleMethod(*ifaceType, memberName, memberAccess.getLocation());
  if (method) {
    memberAccess.setTargetDeclarationId(method->declarationId);
    return Types::Function(method->returnType, method->paramTypes, false,
                           method->isUnsafe);
  }

  logAndThrowError("Unknown member '" + memberName + "' on interface '" +
                       ifaceType->toDisplayString() + "'",
                   memberAccess.getLocation());
}

TypePtr SemanticAnalyzer::resolveTypeParameterMemberType(
    const MemberAccessAST& memberAccess, const TypePtr& objectType,
    const std::string& memberName) {
  // For type parameters, check if there's a narrowed type from _is<T>
  // This allows member access validation during semantic analysis
  if (memberAccess.getObject()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
    // Substitute type parameters first for proper narrowing validation
    TypePtr substitutedType = resolver_.substituteTypeParameters(objectType);
    TypePtr narrowedType =
        ctx_.getNarrowedType(varRef.getName(), substitutedType);
    if (narrowedType) {
      // Recursively dispatch with the narrowed type
      if (narrowedType->isClass()) {
        const auto* classType =
            static_cast<const ClassType*>(narrowedType.get());
        const sun::types::ClassField* field = ctx_.accessibleField(
            *classType, memberName, memberAccess.getLocation());
        if (field) {
          memberAccess.setTargetDeclarationId(field->declarationId);
          return field->type;
        }
        const ClassMethod* method = ctx_.accessibleMethod(
            *classType, memberName, memberAccess.getLocation());
        if (method)
          return Types::Function(method->returnType, method->paramTypes, false,
                                 method->isUnsafe);
        logAndThrowError("Unknown member '" + memberName + "' on class '" +
                             classType->getDisplayName() + "'",
                         memberAccess.getLocation());
      }
      if (narrowedType->isInterface()) {
        const auto* ifaceType =
            static_cast<const sun::types::InterfaceType*>(narrowedType.get());
        const InterfaceField* field = ctx_.accessibleField(
            *ifaceType, memberName, memberAccess.getLocation());
        if (field) {
          memberAccess.setTargetDeclarationId(field->declarationId);
          return field->type;
        }
        const InterfaceMethod* method = ctx_.accessibleMethod(
            *ifaceType, memberName, memberAccess.getLocation());
        if (method)
          return Types::Function(method->returnType, method->paramTypes, false,
                                 method->isUnsafe);
        logAndThrowError("Unknown member '" + memberName + "' on interface '" +
                             ifaceType->toDisplayString() + "'",
                         memberAccess.getLocation());
      }
    }
  }
  // No narrowing. A `<T: ISomething>` constraint is the other way a type
  // parameter can have known members: whatever T turns out to be, it
  // implements that interface, so the interface's members are reachable
  // here and every specialization will have them.
  const auto* param =
      static_cast<const sun::types::TypeParameterType*>(objectType.get());
  if (param->hasConstraint()) {
    const auto& constraint = param->getConstraint();
    auto ifaceType =
        sun::semantic_analysis::type_analysis::isTypeTrait(constraint.name)
            ? nullptr
            : resolver_.resolveConstraintInterface(constraint);
    if (ifaceType) {
      const InterfaceField* field = ctx_.accessibleField(
          *ifaceType, memberName, memberAccess.getLocation());
      if (field) {
        memberAccess.setTargetDeclarationId(field->declarationId);
        return field->type;
      }
      const InterfaceMethod* method = ctx_.accessibleMethod(
          *ifaceType, memberName, memberAccess.getLocation());
      if (method)
        return Types::Function(method->returnType, method->paramTypes, false,
                               method->isUnsafe);
      logAndThrowError("Unknown member '" + memberName +
                           "' on type parameter '" + param->getName() +
                           "', which is constrained to interface '" +
                           constraint.toString() + "'",
                       memberAccess.getLocation());
    }
    // A trait such as `_Numeric`, or `lambda`: it says which types are
    // allowed, not which members they carry.
    logAndThrowError("Cannot access member '" + memberName +
                         "' on type parameter '" + param->getName() +
                         "': its constraint '" + constraint.toString() +
                         "' is a type trait, which promises no members. "
                         "Constrain it to an interface to call methods on "
                         "it.",
                     memberAccess.getLocation());
  }

  logAndThrowError("Cannot access member '" + memberName +
                       "' on unconstrained type parameter '" +
                       objectType->toDisplayString() + "'",
                   memberAccess.getLocation());
}

TypePtr SemanticAnalyzer::resolveMemberType(
    const MemberAccessAST& memberAccess) {
  if (memberAccess.getModuleDeclaration())
    return resolveModuleReference(memberAccess);
  const std::string& memberName = memberAccess.getMemberName();

  // Get the fully-resolved object type:
  // 1. Use resolved type if available, otherwise infer
  // 2. Unwrap references
  // 3. Check for type narrowing from _is<T> guards
  // 4. Unwrap raw_ptr<Class> to Class for member access
  TypePtr objectType = memberAccess.getObject()->getResolvedType();
  if (!objectType &&
      memberAccess.getObject()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    // Generic enum object (Option.None): only valid once analysis resolved
    // the specialization from context
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
    if (!ctx_.currentScope().lookupVariable(varRef.getName()) &&
        ctx_.scope()->lookupGenericEnum(varRef.getName())) {
      if (auto resolved = memberAccess.getResolvedType()) {
        return resolved;
      }
      logAndThrowError("Cannot infer type arguments for '" + varRef.getName() +
                           "." + memberName +
                           "'; add a type annotation to the target",
                       memberAccess.getLocation());
    }
  }
  if (!objectType) {
    objectType = requireResolvedType(*memberAccess.getObject());
  }
  objectType = unwrapRef(objectType);
  // Cached generic fields may carry a parameter from another signature.
  // Use this scope's binding without changing the shared class field.
  if (objectType && objectType->isTypeParameter()) {
    objectType = unwrapRef(resolver_.substituteTypeParameters(objectType));
  }

  if (!objectType) {
    logAndThrowError(
        "Cannot access member '" + memberName + "' on unknown type",
        memberAccess.getLocation());
  }

  // Unwrap raw_ptr<Class> to Class for member access (requires unsafe)
  if (objectType->isRawPointer()) {
    TypePtr pointeeType =
        static_cast<sun::types::RawPointerType*>(objectType.get())
            ->getPointeeType();
    if (pointeeType && pointeeType->isClass()) {
      if (!ctx_.isInUnsafeBlock()) {
        logAndThrowError(
            "Dereferencing 'raw_ptr' can only be done in an unsafe block",
            memberAccess.getLocation());
      }
      objectType = pointeeType;
    }
  }

  // Unwrap static_ptr<Class> to Class for member access
  if (objectType->isStaticPointer()) {
    TypePtr pointeeType =
        static_cast<StaticPointerType*>(objectType.get())->getPointeeType();
    if (pointeeType && pointeeType->isClass()) {
      objectType = pointeeType;
    }
  }

  // Now dispatch based on the resolved object type
  switch (objectType->getKind()) {
    case sun::types::Type::Kind::Module:
      return resolveModuleMemberType(memberAccess, objectType, memberName);
    case sun::types::Type::Kind::Enum: {
      auto* enumType = static_cast<EnumType*>(objectType.get());
      const auto* variant = enumType->getVariant(memberName);
      if (variant) {
        if (variant->hasPayload()) {
          logAndThrowError("Variant '" + memberName + "' of enum '" +
                               enumType->getDisplayName() +
                               "' carries a payload; construct it with '" +
                               enumType->getBaseName() + "." + memberName +
                               "(...)'",
                           memberAccess.getLocation());
        }
        return objectType;  // Enum variant has the enum type
      }
      logAndThrowError("Unknown variant '" + memberName + "' in enum '" +
                           enumType->getDisplayName() + "'",
                       memberAccess.getLocation());
    }

    case sun::types::Type::Kind::Array: {
      // The accessors are methods; the call form is typed by
      // resolveArrayMethodType before the callee is inferred, so reaching
      // here means the property form was written.
      if ((memberName == "ndims" || memberName == "dim")) {
        logAndThrowError("Array has no property '" + memberName +
                             "'; call it: '" + memberName + "(...)'",
                         memberAccess.getLocation());
      }
      logAndThrowError("Array has no member '" + memberName +
                           "'; available: ndims(), dim(i)",
                       memberAccess.getLocation());
    }

    case sun::types::Type::Kind::StaticPointer: {
      // The accessors are methods; the call form is typed by
      // resolveStaticPtrMethodType before the callee is inferred, so reaching
      // here means the property form was written.
      if ((memberName == "length" || memberName == "raw")) {
        logAndThrowError("static_ptr has no property '" + memberName +
                             "'; call '" + memberName + "()'",
                         memberAccess.getLocation());
      }
      logAndThrowError("static_ptr has no member '" + memberName +
                           "'; available: length(), raw()",
                       memberAccess.getLocation());
    }

    case sun::types::Type::Kind::RawPointer: {
      // raw_ptr<T> where T is not a class (class case handled above): a
      // bare pointer has no members; read through it with _load<T> or
      // _to_ref<T>
      logAndThrowError("raw_ptr has no member '" + memberName +
                           "'; read through it with _load<T>(p, i) or "
                           "_to_ref<T>(p)",
                       memberAccess.getLocation());
    }

    case sun::types::Type::Kind::Class:
      return resolveClassMemberType(memberAccess, objectType, memberName);
    case sun::types::Type::Kind::Interface:
      return resolveInterfaceMemberType(memberAccess, objectType, memberName);
    case sun::types::Type::Kind::TypeParameter:
      return resolveTypeParameterMemberType(memberAccess, objectType,
                                            memberName);
    default:
      logAndThrowError("Cannot access member '" + memberName + "' on type '" +
                           objectType->toDisplayString() + "'",
                       memberAccess.getLocation());
  }
}

TypePtr SemanticAnalyzer::resolveModuleReference(const ExprAST& expr) {
  auto id =
      ctx_.results().declarations.findPortable(*expr.getModuleDeclaration());
  auto* module = ctx_.lookupModuleScope(id);
  ctx_.requireModuleAccessible(*module, expr.getLocation());
  return Types::Module(
      static_cast<const ModuleScope&>(*module).qualifiedName.lookupName());
}
TypePtr SemanticAnalyzer::requireResolvedType(const ExprAST& expr) {
  if (auto type = expr.getResolvedType()) return type;
  logAndThrowError(
      "Internal error: expression type was not prepared for inference",
      expr.getLocation());
}

TypePtr SemanticAnalyzer::preparedBlockType(
    const sun::ast::BlockExprAST& block) {
  for (const auto& statement : block.getBody()) {
    if (statement->isReturn()) return requireResolvedType(*statement);
  }
  if (!block.producesValue() || block.isEmpty()) return Types::Void();
  return requireResolvedType(*block.getBody().back());
}
TypePtr SemanticAnalyzer::preparedMatchType(
    const sun::ast::MatchExprAST& match) {
  std::set<int64_t> coveredTags;
  for (const auto& arm : match.getArms()) {
    if (!arm.isWildcard && arm.pattern && arm.pattern->getResolvedType() &&
        arm.pattern->getResolvedType()->isEnum() &&
        !coveredTags.insert(arm.resolvedVariantTag).second)
      continue;
    if (!sun::ast::exprDiverges(*arm.body))
      return unwrapRef(requireResolvedType(*arm.body));
    if (arm.isWildcard) break;
  }
  return Types::Void();
}

}  // namespace sun::semantic_analysis
