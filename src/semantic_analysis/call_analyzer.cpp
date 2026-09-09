// call_analyzer.cpp — Semantic analysis of calls. See call_analyzer.h.

#include "semantic_analysis/call_analyzer.h"

#include <string>

#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/argument_conversion.h"
#include "semantic_analysis/generic_type_arguments.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_rules.h"
#include "support/error.h"

using sun::formatTypeList;
using sun::unwrapRef;
using sun::rules::isAssignableTo;
using sun::rules::tryCoerceIntegerLiteral;

namespace {

// "\n  - trim()\n  - trim(ref HeapAllocator)" — the candidate list shown
// after "No matching overload".
std::string formatCandidates(
    const std::string& name,
    const std::vector<std::vector<sun::TypePtr>>& candidates) {
  std::string out;
  for (const auto& params : candidates) {
    out += "\n  - " + name + "(" + formatTypeList(params) + ")";
  }
  return out;
}

// The resolved types of a call's arguments, in order.
std::vector<sun::TypePtr> resolvedTypesOf(
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  std::vector<sun::TypePtr> types;
  types.reserve(args.size());
  for (const auto& arg : args) types.push_back(arg->getResolvedType());
  return types;
}

// Precompute contextual types for each unsuffixed integer argument.
std::vector<FunctionArgumentType> functionArgumentTypes(
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  std::vector<FunctionArgumentType> types(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    types[i].preferred = args[i]->getResolvedType();
    if (args[i]->getType() != ASTNodeType::NUMBER) continue;
    const auto& number = static_cast<const NumberExprAST&>(*args[i]);
    if (!number.isInteger() || number.hasSuffix()) continue;
    for (const auto& type : {sun::Types::Int8(), sun::Types::Int16(),
                             sun::Types::Int32(), sun::Types::Int64(),
                             sun::Types::UInt8(), sun::Types::UInt16(),
                             sun::Types::UInt32(), sun::Types::UInt64(),
                             sun::Types::Bool()}) {
      if (sun::rules::literalFitsInType(number.getMagnitude(),
                                        number.isNegative(), type->getKind())) {
        types[i].alternatives.push_back(type);
      }
    }
  }
  return types;
}

// What to call the callee in diagnostics: a plain call gives its function
// name, a method call its member name.
std::string calleeDisplayName(const CallExprAST& callExpr) {
  const ExprAST& callee = *callExpr.getCallee();
  if (callee.getType() == ASTNodeType::VARIABLE_REFERENCE) {
    return static_cast<const VariableReferenceAST&>(callee).getName();
  }
  if (callee.getType() == ASTNodeType::MEMBER_ACCESS) {
    return static_cast<const MemberAccessAST&>(callee).getMemberName();
  }
  return "<unknown>";
}

// Whether an argument of one type may be passed to a parameter of another
// when the two are not equal: the implicit conversions a call site allows.
// `calleeIsIntrinsic` unlocks the byte-pointer erasure only intrinsics may
// use.
bool isImplicitlyConvertibleArgument(const sun::TypePtr& argType,
                                     const sun::TypePtr& paramType,
                                     bool calleeIsIntrinsic) {
  // Reference parameter accepts the referenced type directly
  if (paramType->isReference()) {
    auto* refType = static_cast<const sun::ReferenceType*>(paramType.get());
    if (refType->getReferencedType()->equals(*argType)) return true;
    // A borrow of the other mutability: only ref -> const ref
    if (argType->isReference()) {
      auto* argRef = static_cast<const sun::ReferenceType*>(argType.get());
      if (sun::refMutabilityConvertible(*argRef, *refType) &&
          refType->getReferencedType()->equals(*argRef->getReferencedType())) {
        return true;
      }
    }
    // ref array<T> (unsized) accepts any array<T, dims...>
    if (refType->getReferencedType()->isArray() && argType->isArray()) {
      auto* paramArray = static_cast<const sun::ArrayType*>(
          refType->getReferencedType().get());
      auto* argArray = static_cast<const sun::ArrayType*>(argType.get());
      if (paramArray->isUnsized() &&
          paramArray->getElementType()->equals(*argArray->getElementType())) {
        return true;
      }
    }
    // Auto-deref: raw_ptr<T> is compatible with ref T
    if (argType->isRawPointer()) {
      auto* ptrType = static_cast<const sun::RawPointerType*>(argType.get());
      if (ptrType->getPointeeType()->equals(*refType->getReferencedType())) {
        return true;
      }
    }
  }

  // Auto-deref: raw_ptr<T> can be passed where T or ref T is expected
  if (argType->isRawPointer() && !paramType->isRawPointer()) {
    auto* ptrType = static_cast<const sun::RawPointerType*>(argType.get());
    sun::TypePtr pointeeType = ptrType->getPointeeType();
    // For primitives, auto-deref to value is allowed
    if (pointeeType->equals(*paramType) && paramType->isPrimitive() &&
        !paramType->isReference()) {
      return true;
    }
    // For any type, auto-deref to ref is allowed
    if (paramType->isReference()) {
      auto* refType = static_cast<const sun::ReferenceType*>(paramType.get());
      if (pointeeType->equals(*refType->getReferencedType())) return true;
    }
  }

  // Null is compatible with any pointer type
  if (argType->isNullPointer() && paramType->isAnyPointer()) return true;

  // Integer widening: smaller int types can be passed to larger int params
  // i8 -> i16 -> i32 -> i64, u8 -> u16 -> u32 -> u64; float f32 -> f64
  if (argType->isPrimitive() && paramType->isPrimitive()) {
    if ((argType->isInt8() || argType->isInt16() || argType->isInt32()) &&
        paramType->isInt64()) {
      return true;
    }
    if ((argType->isInt8() || argType->isInt16()) && paramType->isInt32()) {
      return true;
    }
    if (argType->isInt8() && paramType->isInt16()) return true;
    if ((argType->isUInt8() || argType->isUInt16() || argType->isUInt32()) &&
        paramType->isUInt64()) {
      return true;
    }
    if ((argType->isUInt8() || argType->isUInt16()) && paramType->isUInt32()) {
      return true;
    }
    if (argType->isUInt8() && paramType->isUInt16()) return true;
    if (argType->isFloat32() && paramType->isFloat64()) return true;
  }

  // static_ptr<T> is compatible with raw_ptr<T>
  if (argType->isStaticPointer() && paramType->isRawPointer()) {
    auto* staticPtr = static_cast<const sun::StaticPointerType*>(argType.get());
    auto* rawPtr = static_cast<const sun::RawPointerType*>(paramType.get());
    if (staticPtr->getPointeeType()->equals(*rawPtr->getPointeeType())) {
      return true;
    }
  }

  // raw_ptr<T> is compatible with byte pointers raw_ptr<i8>/raw_ptr<u8> (like
  // C's void*). Only for intrinsics, to avoid accidental type erasure in user
  // code.
  if (calleeIsIntrinsic && argType->isRawPointer() &&
      paramType->isRawPointer()) {
    auto* paramRawPtr =
        static_cast<const sun::RawPointerType*>(paramType.get());
    if (paramRawPtr->getPointeeType()->isInt8() ||
        paramRawPtr->getPointeeType()->isUInt8()) {
      return true;
    }
  }

  // Class-to-interface: a class C can be passed where interface I is expected
  // if C implements I
  return isAssignableTo(argType, paramType);
}

}  // namespace

// -------------------------------------------------------------------
// analyzeCall
// -------------------------------------------------------------------

void CallAnalyzer::analyzeCall(CallExprAST& callExpr,
                               sun::TypePtr expectedType) {
  // Non-generic intrinsics. The generic ones (_load<T>, _to_ref<T>, ...) are a
  // GenericCallAST and go through analyzeGenericCall, which applies the same
  // predicate.
  auto calleeASTType = callExpr.getCallee()->getType();
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*callExpr.getCallee());
    checkRequiresUnsafeBlock(varRef.getName(), callExpr.getLocation());
  }

  // Enum variant construction: EnumName.Variant(args...) for concrete and
  // generic enums; intercepted before generic callee analysis.
  if (sema_.enums().tryAnalyzeEnumConstruction(callExpr, expectedType)) {
    return;
  }

  std::vector<sun::TypePtr> argTypes =
      analyzeCallArguments(callExpr, expectedType);
  const auto& args = callExpr.getArgs();

  // Work out what is actually being called, and what that means for the
  // arguments: which overload, whether it is a constructor, whether it
  // swallows a pack, and whether its receiver was constant.
  CalleeResolution callee = resolveCallee(callExpr, argTypes);
  CallSignature signature = resolveCallSignature(callExpr, callee, argTypes);
  const std::vector<sun::TypePtr>& paramTypes = signature.paramTypes;

  // Only a plain call can name an intrinsic, so the intrinsic-only
  // conversions key off that form.
  std::string funcName = calleeDisplayName(callExpr);
  bool calleeIsIntrinsic = calleeASTType == ASTNodeType::VARIABLE_REFERENCE &&
                           sun::names::isIntrinsic(funcName);

  // Check argument count. A C-variadic callee fixes only its leading
  // parameters, so extra trailing arguments are allowed.
  bool calleeIsCVariadic = callee.function && callee.function->isCVariadic;
  bool badArgCount = calleeIsCVariadic ? args.size() < paramTypes.size()
                                       : args.size() != paramTypes.size();
  if (signature.known && !callee.takesPack && badArgCount) {
    logAndThrowError("Function '" + funcName + "' expects " +
                         (calleeIsCVariadic ? "at least " : "") +
                         std::to_string(paramTypes.size()) +
                         " arguments, got " + std::to_string(args.size()),
                     callExpr.getLocation());
  }

  sema_.checkPackedRefArguments(args, paramTypes);
  if (signature.known) {
    sema_.checkArgumentPlaces(args, paramTypes, funcName,
                              callExpr.getLocation());
  }

  // An overload chosen from the argument types already fits them; anything
  // else is checked argument by argument.
  if (!callee.function) {
    checkArgumentTypes(callExpr, paramTypes, funcName, calleeIsIntrinsic);
  }

  checkThrowPropagation(callExpr, callee, funcName);

  // Record how each argument reaches its parameter. Codegen carries these
  // out and never compares Sun types at the call boundary itself.
  if (signature.known) {
    callExpr.setArgConversions(sun::conversions::classifyArguments(
        callExpr.getResolvedArgTypes(), paramTypes, calleeIsCVariadic, funcName,
        callExpr.getLocation()));
  }

  // A borrow handed out by a method seen through an immutable receiver may
  // only be read through
  sun::TypePtr resultType = types_.inferType(callExpr);
  if (callee.receiverImmutable) resultType = types_.createConstView(resultType);
  callExpr.setResolvedType(resultType);
}

// The `args...` of a call, analyzed and typed. Arguments go first so that
// overload resolution has real types to match, which means an argument that
// needs a hint — an array literal, an overloaded bound method reference —
// has to get it from a provisional look at the callee before it is analyzed.
std::vector<sun::TypePtr> CallAnalyzer::analyzeCallArguments(
    CallExprAST& callExpr, sun::TypePtr expectedType) {
  auto calleeASTType = callExpr.getCallee()->getType();
  // Get parameter types early for array literal type propagation
  std::vector<sun::TypePtr> expectedParamTypes;
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*callExpr.getCallee());
    // Resolve the name through using imports
    sun::QualifiedName resolved = ctx_.resolveNameWithUsings(varRef.getName());
    // Try to look up function parameters
    auto allFuncs = ctx_.getAllFunctions(resolved.baseName);
    if (!allFuncs.empty()) {
      // Use first overload's param types for type propagation
      expectedParamTypes = allFuncs[0].paramTypes;
    } else {
      // Check if this is a class constructor (use base name for lookup)
      auto classType = ctx_.lookupClass(resolved.baseName);
      if (classType) {
        // Get init method parameters
        if (auto* initMethod = classType->getMethod("init")) {
          expectedParamTypes = initMethod->paramTypes;
        }
      }
    }
  }

  const auto& args = callExpr.getArgs();
  hintArrayLiteralArguments(args, expectedParamTypes);

  // Analyze arguments before the callee. Passing the provisional parameter
  // type lets callable values select an overload and still supplies the
  // existing array-literal and bound-method hints.
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& arg = args[i];
    sun::TypePtr expected;
    if (i < expectedParamTypes.size()) {
      const auto& paramType = expectedParamTypes[i];
      if (arg->getType() == ASTNodeType::MEMBER_ACCESS ||
          (paramType && (paramType->isFunction() || paramType->isLambda()))) {
        expected = paramType;
      }
    }
    sema_.analyzeExpr(const_cast<ExprAST&>(*arg), expected);
  }

  // Expand any variadic pack (`f(args...)`) into concrete typed args before
  // overload resolution, so the types below reflect the real arguments.
  expandPackArguments(callExpr.getArgsMutable());

  return resolvedTypesOf(callExpr.getArgs());
}

CallAnalyzer::CalleeResolution CallAnalyzer::resolveCallee(
    CallExprAST& callExpr, const std::vector<sun::TypePtr>& argTypes) {
  ExprAST& callee = const_cast<ExprAST&>(*callExpr.getCallee());
  switch (callee.getType()) {
    case ASTNodeType::VARIABLE_REFERENCE:
      return resolveNamedCallee(
          callExpr, static_cast<VariableReferenceAST&>(callee), argTypes);
    case ASTNodeType::MEMBER_ACCESS:
      return resolveMemberCallee(
          callExpr, static_cast<MemberAccessAST&>(callee), argTypes);
    default:
      // Not a simple variable reference or method call - analyze the callee
      // expression
      sema_.analyzeExpr(callee);
      return {};
  }
}

// For function calls by name, do overload resolution before analyzing the
// callee. This avoids errors for overloaded functions referenced by name.
CallAnalyzer::CalleeResolution CallAnalyzer::resolveNamedCallee(
    CallExprAST& callExpr, VariableReferenceAST& varRef,
    const std::vector<sun::TypePtr>& argTypes) {
  CalleeResolution out;
  // Resolve the name through using imports (e.g., Vec -> sun_Vec)
  sun::QualifiedName resolved = ctx_.resolveNameWithUsings(varRef.getName());

  // Store the qualified name so codegen doesn't need to do name resolution
  if (resolved.mangled() != varRef.getName()) {
    varRef.setQualifiedName(resolved);
  }

  const auto& args = callExpr.getArgs();
  const auto lookupTypes = functionArgumentTypes(args);
  out.function = ctx_.lookupFunction(resolved.baseName, lookupTypes,
                                      callExpr.getLocation());
  if (out.function) {
    for (size_t i = 0; i < out.function->paramTypes.size(); ++i) {
      tryCoerceIntegerLiteral(args[i].get(), out.function->paramTypes[i]);
    }
    checkExternCallAllowed(*out.function, varRef.getName(),
                           callExpr.getLocation());
    varRef.setResolvedType(sun::Types::Function(out.function->returnType,
                                                out.function->paramTypes,
                                                out.function->canThrow));
    // Set qualified name from the resolved function (handles import scopes)
    if (!out.function->qualifiedName.empty()) {
      varRef.setQualifiedName(out.function->qualifiedName);
    }
    return out;
  }

  // A class constructor call, ClassName(args...): a stack-allocated instance
  out.classType = ctx_.lookupClass(resolved.baseName);
  if (out.classType) {
    varRef.setResolvedType(out.classType);
    return out;
  }

  // A generic function called without type arguments — `identity(42)`. The
  // arguments say what T is, so instantiate that specialization and let the
  // call resolve to it like any other named function.
  if (const GenericFunctionInfo* genericFunc =
          ctx_.lookupGenericFunction(resolved.baseName)) {
    GenericCallTarget target = resolveGenericCallTarget(
        *genericFunc, argTypes, /*writtenTypeArgs=*/{}, varRef.getName(),
        callExpr.getLocation());
    out.takesPack = target.takesPack;
    if (target.specialized) {
      out.function = target.specialized->asFunctionInfo();
      varRef.setQualifiedName(target.specialized->qualifiedName);
    }
    varRef.setResolvedType(target.calleeType);
    return out;
  }

  // Overloads exist under this name but none took these arguments: say
  // which ones there are.
  auto allOverloads = ctx_.getAllFunctions(resolved.baseName);
  if (!allOverloads.empty()) {
    std::string argTypesStr;
    for (size_t i = 0; i < argTypes.size(); ++i) {
      if (i > 0) argTypesStr += ", ";
      argTypesStr += argTypes[i] ? argTypes[i]->toDisplayString() : "unknown";
    }
    std::string overloadsStr;
    for (const auto& overload : allOverloads) {
      overloadsStr += "\n  - " + resolved.baseName + "(";
      for (size_t i = 0; i < overload.paramTypes.size(); ++i) {
        if (i > 0) overloadsStr += ", ";
        overloadsStr += overload.paramTypes[i]
                            ? overload.paramTypes[i]->toDisplayString()
                            : "unknown";
      }
      if (overload.isCVariadic) {
        overloadsStr += overload.paramTypes.empty() ? "..." : ", ...";
      }
      overloadsStr += ")";
    }
    logAndThrowError("No matching overload of '" + resolved.baseName +
                         "' for argument types (" + argTypesStr +
                         "). Available overloads:" + overloadsStr,
                     callExpr.getLocation());
  }

  // Not a function or class - analyze normally (a variable holding a
  // callable, or an unknown name)
  sema_.analyzeExpr(varRef);
  return out;
}

CallAnalyzer::CalleeResolution CallAnalyzer::resolveMemberCallee(
    CallExprAST& callExpr, MemberAccessAST& memberAccess,
    const std::vector<sun::TypePtr>& argTypes) {
  // First analyze the object expression to get its type
  sema_.analyzeExpr(const_cast<ExprAST&>(*memberAccess.getObject()));

  // Get object type (unwrap references)
  sun::TypePtr objectType = memberAccess.getObject()->getResolvedType();
  if (!objectType) {
    objectType = types_.inferType(*memberAccess.getObject());
  }
  objectType = unwrapRef(objectType);

  if (objectType && objectType->isClass()) {
    return resolveMethodCallee(memberAccess, objectType, argTypes);
  }
  if (const FunctionInfo* modFunc =
          resolveModuleQualifiedCall(memberAccess, objectType, argTypes)) {
    // Module-qualified call: the overload is chosen from the argument
    // types here. types_.inferType() alone would only see the first overload.
    memberAccess.setResolvedType(sun::Types::Function(
        modFunc->returnType, modFunc->paramTypes, modFunc->canThrow));
    return {};
  }
  // Module-qualified generic call: type arguments written or inferred, and
  // the callee pinned to its specialization.
  if (auto generic = resolveModuleQualifiedGenericCall(memberAccess,
                                                       objectType, argTypes)) {
    return *generic;
  }

  CalleeResolution out;
  if (auto* staticPtr = types_.asNonClassStaticPtr(objectType)) {
    // static_ptr<T> builtin methods: length(), raw()
    memberAccess.setResolvedType(types_.inferStaticPtrMethodType(
        *staticPtr, memberAccess.getMemberName(), callExpr.getArgs().size(),
        memberAccess.getLocation()));
  } else if (objectType && objectType->isArray()) {
    // Array builtin methods: ndims(), dim(i)
    memberAccess.setResolvedType(types_.inferArrayMethodType(
        memberAccess.getMemberName(), argTypes, memberAccess.getLocation()));
  } else {
    // Not a class type (interface, module, ptr-to-class, builtin...).
    // Set the type directly (the object is already analyzed) instead of
    // analyzeExpr, so a ptr-to-class method callee is not converted to a
    // bound-method lambda — call position requires a FunctionType.
    memberAccess.setResolvedType(types_.inferType(memberAccess));
    if (objectType && objectType->isInterface()) {
      const auto* iface =
          static_cast<const sun::InterfaceType*>(objectType.get());
      if (const auto* method = iface->getMethod(memberAccess.getMemberName())) {
        out.receiverImmutable = sema_.checkMethodReceiver(
            *memberAccess.getObject(), method->name, method->isConst,
            /*isConstructor=*/false, memberAccess.getLocation());
      }
    }
  }
  return out;
}

CallAnalyzer::CalleeResolution CallAnalyzer::resolveMethodCallee(
    MemberAccessAST& memberAccess, const sun::TypePtr& objectType,
    const std::vector<sun::TypePtr>& argTypes) {
  CalleeResolution out;
  const auto* classType = static_cast<const sun::ClassType*>(objectType.get());
  const std::string& methodName = memberAccess.getMemberName();
  const Position& loc = memberAccess.getLocation();

  // A non-const method needs a mutable receiver, and a constant one makes
  // any `ref T` result read-only.
  auto checkReceiver = [&](const sun::ClassMethod& method) {
    out.receiverImmutable = sema_.checkMethodReceiver(
        *memberAccess.getObject(), methodName, method.isConst,
        method.isConstructor, loc);
  };

  // Generic method ending in an `args...` pack (e.g.
  // allocator.create<Point>(...)): specialize HERE, where the actual call
  // argument types are known, so overloaded constructors resolve and the
  // specialization is keyed (mangled) by the pack's arg types. The
  // inferType trigger defers variadic methods to this path.
  FunctionAST* genericMethod =
      generics_.findGenericMethodAST(classType, methodName);
  bool variadicMethod =
      genericMethod && genericMethod->getProto().hasVariadicParam();
  const sun::ClassField* callableField = classType->getField(methodName);
  if (callableField && callableField->type &&
      callableField->type->isCallable()) {
    memberAccess.setResolvedType(callableField->type);
    return out;
  }

  if (variadicMethod && memberAccess.hasTypeArguments()) {
    out.takesPack = true;
    std::vector<sun::TypePtr> typeArgPtrs;
    for (const auto& ta : memberAccess.getTypeArguments()) {
      typeArgPtrs.push_back(types_.typeAnnotationToType(*ta));
    }
    memberAccess.setResolvedTypeArgs(typeArgPtrs);
    // Only what is left after the method's fixed parameters fills the
    // pack; `create<T>(args...)` has none, but `(x: i32, args...)` does.
    std::vector<sun::TypePtr> packArgTypes = *generics_.splitPackArgTypes(
        genericMethod->getProto(), argTypes, methodName, loc);
    memberAccess.setResolvedVariadicArgTypes(packArgTypes);

    auto mutableClassType =
        std::static_pointer_cast<sun::ClassType>(objectType);
    // Point the call at the specialization, under the name given where
    // it was instantiated (pack suffix included).
    if (auto specialized = generics_.instantiateGenericMethod(
            mutableClassType, methodName, typeArgPtrs, packArgTypes)) {
      memberAccess.setQualifiedName(specialized->getProto().getQualifiedName());
    }

    if (const sun::ClassMethod* method =
            ctx_.accessibleMethod(*classType, methodName, loc)) {
      memberAccess.setResolvedType(
          sun::Types::Function(method->returnType, method->paramTypes));
      checkReceiver(*method);
    }
    return out;
  }

  if (genericMethod && !variadicMethod) {
    // A generic method: whatever type arguments the call leaves out are
    // inferred from its arguments, then types_.inferType() instantiates the
    // specialization from the complete list (and does the same when all
    // of them were written).
    const sun::ClassMethod* method =
        ctx_.accessibleMethod(*classType, methodName, loc);
    std::vector<sun::TypePtr> written = types_.resolveTypeArguments(
        memberAccess.getTypeArguments(), loc, "generic method call");
    if (method && written.size() < method->typeParameters.size()) {
      memberAccess.setResolvedTypeArgs(sun::generics::inferMethodTypeArguments(
          *method, argTypes, classType->getDisplayName() + "." + methodName,
          loc, written));
    }
    memberAccess.setResolvedType(types_.inferType(memberAccess));
    if (method) checkReceiver(*method);
    return out;
  }

  // Try to find a method overload matching the argument types
  if (const sun::ClassMethod* method =
          ctx_.accessibleMethodForArgs(*classType, methodName, argTypes, loc)) {
    memberAccess.setResolvedType(
        sun::Types::Function(method->returnType, method->paramTypes));
    checkReceiver(*method);
    return out;
  }

  // No overload took these arguments. When the mismatch is the argument
  // *count*, say so here: the fallback below picks an arbitrary overload, and
  // a zero-parameter one leaves nothing for the arity check to compare
  // against (issue #87).
  reportNoMethodForArgCount(*classType, methodName, argTypes, loc);
  // Fall back to first method with this name (will error on type mismatch).
  // Set the type directly (the object is already analyzed) instead of
  // analyzeExpr, so the callee is not converted to a bound-method lambda —
  // call position requires a FunctionType.
  memberAccess.setResolvedType(types_.inferType(memberAccess));
  return out;
}

CallAnalyzer::CallSignature CallAnalyzer::resolveCallSignature(
    CallExprAST& callExpr, CalleeResolution& callee,
    const std::vector<sun::TypePtr>& argTypes) {
  sun::TypePtr calleeType = callExpr.getCallee()->getResolvedType();
  if (!calleeType) {
    calleeType = types_.inferType(*callExpr.getCallee());
  }

  // A module-qualified constructor call (`m.Point(...)`) resolves the callee
  // to the class type; treat it like `Point(...)` below. Only a member of a
  // module names a class this way — a method that returns a class, such as
  // `t.join()` on a `Thread<Point>`, has the same callee type but is a call,
  // not a construction.
  if (!callee.classType && calleeType && calleeType->isClass() &&
      callExpr.getCallee()->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& calleeMember =
        static_cast<const MemberAccessAST&>(*callExpr.getCallee());
    sun::TypePtr ownerType = calleeMember.getObject()->getResolvedType();
    if (!ownerType) ownerType = types_.inferType(*calleeMember.getObject());
    if (ownerType && ownerType->isModule()) {
      callee.classType = std::static_pointer_cast<sun::ClassType>(calleeType);
    }
  }

  CallSignature signature;
  if (callee.function) {
    signature.paramTypes = callee.function->paramTypes;
    signature.known = true;
  } else if (auto* function = sun::tryGetType<sun::FunctionType>(calleeType)) {
    signature.paramTypes = function->getParamTypes();
    signature.known = true;
  } else if (auto* lambda = sun::tryGetType<sun::LambdaType>(calleeType)) {
    signature.paramTypes = lambda->getParamTypes();
    signature.known = true;
  } else if (callee.classType && callee.classType->isClass()) {
    if (auto params = resolveConstructorParams(*callee.classType, argTypes,
                                               callExpr.getLocation())) {
      signature.paramTypes = std::move(*params);
      signature.known = true;
    }
  }
  return signature;
}

void CallAnalyzer::checkArgumentTypes(
    CallExprAST& callExpr, const std::vector<sun::TypePtr>& paramTypes,
    const std::string& funcName, bool calleeIsIntrinsic) {
  const auto& args = callExpr.getArgs();
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    sun::TypePtr argType = args[i]->getResolvedType();
    const sun::TypePtr& paramType = paramTypes[i];

    // Unbound template arguments are checked again with concrete types at
    // specialization, just as they are in argument conversion classification.
    if (sun::generics::mentionsTypeParameter(argType) ||
        sun::generics::mentionsTypeParameter(paramType)) {
      continue;
    }

    // Try to coerce integer literal to parameter type
    if (tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                                paramType)) {
      argType = paramType;
    }

    if (!argType || !paramType || paramType->equals(*argType)) continue;
    if (isImplicitlyConvertibleArgument(argType, paramType,
                                        calleeIsIntrinsic)) {
      continue;
    }

    // The common way to land here now: handing a borrowed element to a
    // by-value parameter. Say what to do about it.
    std::string hint;
    if (argType->isReference() && !paramType->isReference() &&
        !sun::typeCopiesByRead(paramType)) {
      hint = ". It is borrowed, and a '" + paramType->toDisplayString() +
             "' cannot be read out of a borrow: take the parameter by "
             "'ref', pass a clone(), or move the value out first "
             "(take()/pop()/remove() on a container)";
    }
    logAndThrowError("Type mismatch in argument " + std::to_string(i + 1) +
                         " of call to '" + funcName + "': expected " +
                         paramType->toDisplayString() + ", got " +
                         argType->toDisplayString() + hint,
                     callExpr.getLocation());
  }
}

void CallAnalyzer::checkThrowPropagation(const CallExprAST& callExpr,
                                         const CalleeResolution& callee,
                                         const std::string& funcName) {
  bool calleeThrows = callee.function && callee.function->canThrow;
  if (!calleeThrows) {
    sun::TypePtr calleeType = callExpr.getCallee()->getResolvedType();
    if (auto* function = sun::tryGetType<sun::FunctionType>(calleeType)) {
      calleeThrows = function->canThrow();
    } else if (auto* lambda = sun::tryGetType<sun::LambdaType>(calleeType)) {
      calleeThrows = lambda->canThrow();
    }
  }
  if (calleeThrows && !ctx_.isInTryBlock() && !ctx_.isInThrowingFunction()) {
    logAndThrowError("Call to throwing function '" + funcName +
                         "' must be in a try block or in a function declared "
                         "with ', IError'",
                     callExpr.getLocation());
  }
}

// -------------------------------------------------------------------
// Shared by the call forms
// -------------------------------------------------------------------

std::optional<std::vector<sun::TypePtr>> CallAnalyzer::resolveConstructorParams(
    const sun::ClassType& classType, const std::vector<sun::TypePtr>& argTypes,
    const Position& loc) {
  if (const auto* initMethod =
          ctx_.accessibleMethodForArgs(classType, "init", argTypes, loc)) {
    return initMethod->paramTypes;
  }
  if (!classType.getMethod("init")) {
    if (argTypes.empty()) return std::nullopt;
    // No init at all, but arguments were supplied. Field-wise construction
    // is spelled with a struct literal, where each field is named: relying
    // on declaration order would silently change meaning if two same-typed
    // fields were ever reordered.
    logAndThrowError(
        "Class '" + classType.toDisplayString() +
            "' declares no 'init', so it cannot be constructed positionally."
            " Use a struct literal naming each field: `var x: " +
            classType.toDisplayString() + " = { ... };`",
        loc);
  }

  // The class declares one or more init methods but none are compatible
  // with the supplied arguments. List what it does declare — with overloads,
  // "no match" alone leaves the caller guessing which one they nearly hit.
  std::string argList;
  for (size_t i = 0; i < argTypes.size(); ++i) {
    if (i > 0) argList += ", ";
    argList += argTypes[i] ? argTypes[i]->toDisplayString() : "?";
  }
  std::string candidates;
  for (const auto& method : classType.getMethods()) {
    if (method.name != "init") continue;
    std::string params;
    for (size_t i = 0; i < method.paramTypes.size(); ++i) {
      if (i > 0) params += ", ";
      params += method.paramTypes[i] ? method.paramTypes[i]->toDisplayString()
                                     : "?";
    }
    candidates += "\n       candidate: init(" + params + ")";
  }
  logAndThrowError("No matching constructor for '" +
                       classType.toDisplayString() + "' with arguments (" +
                       argList + ")" + candidates,
                   loc);
}

void CallAnalyzer::hintArrayLiteralArguments(
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::TypePtr>& paramTypes) {
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (args[i]->getType() != ASTNodeType::ARRAY_LITERAL) continue;
    // `ref array<T>` hints the same as `array<T>`
    sun::TypePtr paramType = unwrapRef(paramTypes[i]);
    if (paramType && paramType->isArray()) {
      const_cast<ExprAST&>(*args[i]).setResolvedType(paramType);
    }
  }
}

void CallAnalyzer::expandPackArguments(
    std::vector<std::unique_ptr<ExprAST>>& args) {
  auto* fnScope = ctx_.currentFunctionScope();
  if (!fnScope || !fnScope->variadicParam) return;
  const auto& [packName, types] = *fnScope->variadicParam;

  // Is there a pack expansion for this function's variadic param to expand?
  auto isPack = [&](const std::unique_ptr<ExprAST>& a) {
    return a->getType() == ASTNodeType::PACK_EXPANSION &&
           static_cast<const PackExpansionAST&>(*a).getPackName() == packName;
  };
  bool hasPack = false;
  for (const auto& a : args) {
    if (isPack(a)) {
      hasPack = true;
      break;
    }
  }
  if (!hasPack) return;

  // Rewrite `args...` into concrete, already-typed references to the elements
  // the pack was materialized as ("args.0", "args.1", ...). Other args pass
  // through unchanged.
  std::vector<std::unique_ptr<ExprAST>> rebuilt;
  rebuilt.reserve(args.size() + types.size());
  for (auto& a : args) {
    if (isPack(a)) {
      for (size_t i = 0; i < types.size(); ++i) {
        auto vref = std::make_unique<VariableReferenceAST>(packName + "." +
                                                           std::to_string(i));
        vref->setResolvedType(types[i]);
        rebuilt.push_back(std::move(vref));
      }
    } else {
      rebuilt.push_back(std::move(a));
    }
  }
  args = std::move(rebuilt);
}

SymbolMatch CallAnalyzer::findModuleCallee(
    const MemberAccessAST& memberAccess, const sun::TypePtr& objectType,
    SymbolKind kind, const std::vector<sun::TypePtr>* argTypes) const {
  if (!objectType || !objectType->isModule()) return {};
  auto* moduleType = static_cast<sun::ModuleType*>(objectType.get());
  return ctx_.findSymbolInModule(moduleType->getModulePath(),
                                 memberAccess.getMemberName(), kind, argTypes);
}

const FunctionInfo* CallAnalyzer::resolveModuleQualifiedCall(
    const MemberAccessAST& memberAccess, const sun::TypePtr& objectType,
    const std::vector<sun::TypePtr>& argTypes) const {
  SymbolMatch match = findModuleCallee(memberAccess, objectType,
                                       SymbolKind::Function, &argTypes);
  if (!match || !match.functionInfo) return nullptr;

  checkExternCallAllowed(*match.functionInfo, memberAccess.getMemberName(),
                         memberAccess.getLocation());
  memberAccess.setQualifiedName(match.functionInfo->qualifiedName);
  return match.functionInfo;
}

std::optional<CallAnalyzer::CalleeResolution>
CallAnalyzer::resolveModuleQualifiedGenericCall(
    const MemberAccessAST& memberAccess, const sun::TypePtr& objectType,
    const std::vector<sun::TypePtr>& argTypes) {
  SymbolMatch match = findModuleCallee(memberAccess, objectType,
                                       SymbolKind::GenericFunction);
  if (!match || !match.genericFunctionInfo) return std::nullopt;

  auto loc = memberAccess.getLocation();
  GenericCallTarget target = resolveGenericCallTarget(
      *match.genericFunctionInfo, argTypes,
      types_.resolveTypeArguments(memberAccess.getTypeArguments(), loc,
                                  "generic function call"),
      memberAccess.getMemberName(), loc);
  memberAccess.setResolvedTypeArgs(target.typeArgs);
  if (target.specialized) {
    // Codegen calls the name recorded here; it never spells one itself.
    memberAccess.setQualifiedName(target.specialized->qualifiedName);
  }
  memberAccess.setResolvedType(target.calleeType);

  CalleeResolution out;
  out.takesPack = target.takesPack;
  return out;
}

CallAnalyzer::GenericCallTarget CallAnalyzer::resolveGenericCallTarget(
    const GenericFunctionInfo& genericInfo,
    const std::vector<sun::TypePtr>& argTypes,
    const std::vector<sun::TypePtr>& writtenTypeArgs,
    const std::string& displayName, std::optional<Position> loc) {
  GenericCallTarget target;
  target.typeArgs = writtenTypeArgs;
  if (target.typeArgs.size() < genericInfo.typeParameters.size()) {
    target.typeArgs = sun::generics::inferGenericTypeArguments(
        genericInfo, argTypes, displayName, loc, writtenTypeArgs);
  }

  bool hasPack =
      genericInfo.AST && genericInfo.AST->getProto().hasVariadicParam();
  if (generics_.templateStillAbstract(genericInfo, target.typeArgs)) {
    // The substituted signature lists the fixed parameters only, so a pack
    // callee's extra arguments must not be counted against it yet.
    target.takesPack = hasPack;
    target.calleeType =
        generics_.genericFunctionSignature(genericInfo, target.typeArgs);
    return target;
  }

  std::optional<std::vector<sun::TypePtr>> packArgTypes;
  if (hasPack) {
    packArgTypes = generics_.splitPackArgTypes(genericInfo.AST->getProto(),
                                               argTypes, displayName, loc);
  }
  target.specialized = generics_.requireGenericSpecialization(
      genericInfo, target.typeArgs, displayName, loc, packArgTypes);
  target.calleeType = target.specialized->functionType();
  return target;
}

void CallAnalyzer::reportNoMethodForArgCount(
    const sun::ClassType& cls, const std::string& name,
    const std::vector<sun::TypePtr>& argTypes, const Position& loc) const {
  std::vector<std::vector<sun::TypePtr>> candidates;
  for (const auto& method : cls.getMethods()) {
    if (method.name != name) continue;
    // A generic method's recorded parameters are the uninstantiated ones, so
    // their count is not something to hold the call to.
    if (method.isGeneric()) return;
    if (method.paramTypes.size() == argTypes.size()) return;
    candidates.push_back(method.paramTypes);
  }
  if (candidates.empty()) return;

  logAndThrowError(
      "No matching overload of '" + name + "' for argument types (" +
          formatTypeList(argTypes) +
          "). Available overloads:" + formatCandidates(name, candidates),
      loc);
}

void CallAnalyzer::checkExternCallAllowed(const FunctionInfo& info,
                                          const std::string& displayName,
                                          const Position& loc) const {
  if (!info.isCExtern || ctx_.isInUnsafeBlock()) return;
  logAndThrowError(
      "Calling extern function '" + displayName +
          "' requires an unsafe block: C code is outside the borrow "
          "checker's guarantees. Wrap the call in `unsafe { ... }`, or "
          "expose it through a safe Sun wrapper.",
      loc);
}

void CallAnalyzer::checkRequiresUnsafeBlock(const std::string& name,
                                            const Position& loc) const {
  if (ctx_.isInUnsafeBlock() || !sun::requiresUnsafeBlock(name)) return;
  logAndThrowError(
      "'" + name +
          "' reads or writes memory nothing has checked, so it can only be "
          "used in an unsafe block. Wrap the call in `unsafe { ... }`, or "
          "expose it through a safe Sun wrapper.",
      loc);
}

// -------------------------------------------------------------------
// `name<T>(args)`: intrinsics, generic functions, generic classes
// -------------------------------------------------------------------

void CallAnalyzer::analyzeGenericCall(GenericCallAST& genericCall) {
  const std::string& funcName = genericCall.getFunctionName();

  // Resolve the function/class name through using imports
  sun::QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  // Resolve type arguments to sun::TypePtr
  std::vector<sun::TypePtr> typeArgs;
  for (const auto& ta : genericCall.getTypeArguments()) {
    typeArgs.push_back(types_.typeAnnotationToType(*ta));
  }

  // Store resolved type arguments on the AST for codegen
  genericCall.setResolvedTypeArgs(typeArgs);

  // Validate type args
  for (auto& typeArg : typeArgs) {
    sema_.validateTypeParameter(typeArg, genericCall);
  }

  // Dispatch based on call type: intrinsic, generic class, or generic
  // function
  if (sun::isIntrinsic(funcName)) {
    checkRequiresUnsafeBlock(funcName, genericCall.getLocation());
    analyzeIntrinsicCall(genericCall);
  } else if (ctx_.lookupGenericClass(lookupName)) {
    analyzeGenericClassConstruction(genericCall);
  } else if (ctx_.lookupGenericFunction(lookupName)) {
    analyzeGenericFunctionCall(genericCall);
  } else {
    logAndThrowError("Unknown generic function or class '" + funcName + "'",
                     genericCall.getLocation());
  }
}

void CallAnalyzer::analyzeIntrinsicCall(GenericCallAST& genericCall) {
  // Intrinsics are handled at codegen time - just analyze arguments
  for (const auto& arg : genericCall.getArgs()) {
    sema_.analyzeExpr(const_cast<ExprAST&>(*arg));
  }
  // Expand any variadic pack into concrete typed args (e.g. _init<T>(p,
  // args...))
  expandPackArguments(genericCall.getArgsMutable());
  genericCall.setResolvedType(types_.inferGenericCallType(genericCall));

  // _spawn is the one intrinsic that passes its arguments on to something
  // else — the spawned lambda — so, like any other call, how each argument
  // reaches its parameter is decided here rather than in codegen. A compound
  // argument moves: the thread owns it from the moment it starts.
  if (genericCall.getFunctionName() == "_spawn") {
    recordSpawnArgumentConversions(genericCall);
  }
  // _init likewise forwards its arguments to the constructor they select.
  if (genericCall.getFunctionName() == "_init") {
    recordInitArgumentConversions(genericCall);
  }
}

// Argument 0 is the destination pointer and stands in for itself; the rest
// fill the parameters of the `init` overload they match. Without a matching
// overload (no class, no constructor) each argument is handed over as itself,
// and codegen reports the missing constructor when arguments are present.
void CallAnalyzer::recordInitArgumentConversions(GenericCallAST& genericCall) {
  const auto& args = genericCall.getArgs();
  if (args.empty()) return;  // codegen reports the missing pointer

  std::vector<sun::TypePtr> argTypes = resolvedTypesOf(args);
  std::vector<sun::TypePtr> ctorArgTypes(argTypes.begin() + 1, argTypes.end());

  const sun::ClassMethod* init = nullptr;
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  if (!typeArgs.empty() && typeArgs[0] && typeArgs[0]->isClass()) {
    init = static_cast<const sun::ClassType&>(*typeArgs[0])
               .getMethodForArgs("init", ctorArgTypes);
  }

  std::vector<sun::TypePtr> paramTypes{argTypes[0]};
  for (size_t i = 0; i < ctorArgTypes.size(); ++i) {
    paramTypes.push_back(init ? init->paramTypes[i] : ctorArgTypes[i]);
  }
  genericCall.setArgConversions(sun::conversions::classifyArguments(
      argTypes, paramTypes, /*cVariadic=*/false, "_init",
      genericCall.getLocation()));
}

// The callee is argument 0 and is taken apart rather than passed on, so it
// stands in for itself; everything after it fills the callee's parameters.
// F may be a lambda or a named-function value.
void CallAnalyzer::recordSpawnArgumentConversions(GenericCallAST& genericCall) {
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  auto* lambda = typeArgs.empty()
                     ? nullptr
                     : sun::tryGetType<sun::LambdaType>(typeArgs[0]);
  auto* namedFn = typeArgs.empty()
                      ? nullptr
                      : sun::tryGetType<sun::FunctionType>(typeArgs[0]);
  if (!lambda && !namedFn) {
    logAndThrowError("_spawn<F> requires a lambda or function type argument",
                     genericCall.getLocation());
  }

  // The trampoline that runs the thread has no unwind handling, so an error
  // escaping the spawned function would take the process down mid-unwind.
  if (lambda ? lambda->canThrow() : namedFn->canThrow()) {
    logAndThrowError(
        "a spawned function must not throw; catch errors inside it and "
        "return them as part of its result",
        genericCall.getLocation());
  }

  const auto& args = genericCall.getArgs();
  std::vector<sun::TypePtr> paramTypes{typeArgs[0]};
  const auto& calleeParams =
      lambda ? lambda->getParamTypes() : namedFn->getParamTypes();
  for (const auto& param : calleeParams) {
    paramTypes.push_back(param);
  }
  if (args.size() != paramTypes.size()) {
    logAndThrowError(
        "_spawn<F> takes the function and one argument per parameter it "
        "declares: " +
            std::to_string(paramTypes.size()) + " in all, got " +
            std::to_string(args.size()),
        genericCall.getLocation());
  }

  // Whatever the thread takes over cannot be used here afterwards.
  for (const auto& arg : args) {
    sema_.checkMoveSource(*arg, genericCall.getLocation());
  }
  genericCall.setArgConversions(sun::conversions::classifyArguments(
      resolvedTypesOf(args), paramTypes, /*cVariadic=*/false, "_spawn",
      genericCall.getLocation()));
}

void CallAnalyzer::analyzeGenericFunctionCall(GenericCallAST& genericCall) {
  const std::string& funcName = genericCall.getFunctionName();
  const auto& args = genericCall.getArgs();

  // Resolve the function name through using imports
  sun::QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genFuncInfo = ctx_.lookupGenericFunction(lookupName);
  if (!genFuncInfo) {
    logAndThrowError("Unknown generic function '" + funcName + "'",
                     genericCall.getLocation());
  }

  // Store the generic function AST on the call node for codegen
  genericCall.setGenericFunctionAST(genFuncInfo->AST);

  // The callee's own signature, which says whether it ends in a pack.
  const PrototypeAST* calleeProto =
      genFuncInfo->AST ? &genFuncInfo->AST->getProto() : nullptr;
  bool calleeTakesPack = calleeProto && calleeProto->hasVariadicParam();

  // A call may name only the leading type parameters — `f<i32>(x)` for
  // `f<T, U>` — and leave the rest to the arguments, as a call with no type
  // arguments does. That needs the argument types first, and so does a pack:
  // its element types are what the specialization is keyed on.
  bool argsAnalyzed = false;
  std::vector<sun::TypePtr> argTypes;
  if (calleeTakesPack || genericCall.getResolvedTypeArgs().size() <
                             genFuncInfo->typeParameters.size()) {
    for (const auto& arg : args) {
      sema_.analyzeExpr(const_cast<ExprAST&>(*arg));
    }
    // One template forwarding its own pack into another: `g<T>(args...)`.
    if (calleeTakesPack) expandPackArguments(genericCall.getArgsMutable());
    argTypes = resolvedTypesOf(args);
    argsAnalyzed = true;
    if (genericCall.getResolvedTypeArgs().size() <
        genFuncInfo->typeParameters.size()) {
      std::vector<sun::TypePtr> given = genericCall.getResolvedTypeArgs();
      genericCall.setResolvedTypeArgs(sun::generics::inferGenericTypeArguments(
          *genFuncInfo, argTypes, funcName, genericCall.getLocation(), given));
    }
  }
  const auto& typeArgs = genericCall.getResolvedTypeArgs();

  // Everything past the fixed parameters fills the pack.
  std::optional<std::vector<sun::TypePtr>> packArgTypes;
  if (calleeTakesPack) {
    packArgTypes = generics_.splitPackArgTypes(*calleeProto, argTypes, funcName,
                                               genericCall.getLocation());
  }

  // Only instantiate if all type arguments are concrete. Inside a generic
  // function where T is still a type parameter, no real specialization can be
  // made yet - it is created when the outer generic is instantiated with
  // concrete types.
  std::vector<sun::TypePtr> expectedParamTypes;
  bool allConcrete = !generics_.templateStillAbstract(*genFuncInfo, typeArgs);
  if (allConcrete) {
    SpecializedFunctionInfo specializedFunc =
        generics_.requireGenericSpecialization(*genFuncInfo, typeArgs, funcName,
                                               genericCall.getLocation(),
                                               packArgTypes);
    // Fixed parameters followed by the pack's elements, so the checks below
    // line up positionally with the expanded argument list.
    expectedParamTypes = specializedFunc.paramTypes;
    // Record the name so codegen calls exactly what was instantiated
    genericCall.setSpecializationName(specializedFunc.qualifiedName);
  }

  hintArrayLiteralArguments(args, expectedParamTypes);
  if (!argsAnalyzed) {
    for (const auto& arg : args) {
      sema_.analyzeExpr(const_cast<ExprAST&>(*arg));
    }
  }

  // Coerce integer literals to the instantiated parameter types (there is
  // exactly one signature, so a non-fitting literal is a hard error)
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                            expectedParamTypes[i], /*throwOnFail=*/true);
  }
  sema_.checkArgumentPlaces(args, expectedParamTypes, funcName,
                            genericCall.getLocation());

  if (allConcrete) {
    genericCall.setArgConversions(sun::conversions::classifyArguments(
        resolvedTypesOf(args), expectedParamTypes, /*cVariadic=*/false,
        funcName, genericCall.getLocation()));
  }

  genericCall.setResolvedType(types_.inferGenericCallType(genericCall));
}

void CallAnalyzer::analyzeGenericClassConstruction(
    GenericCallAST& genericCall) {
  const std::string& funcName = genericCall.getFunctionName();
  const auto& args = genericCall.getArgs();
  const auto& typeArgs = genericCall.getResolvedTypeArgs();

  // Resolve the class name through using imports
  sun::QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  if (!ctx_.lookupGenericClass(lookupName)) {
    logAndThrowError("Unknown generic class '" + funcName + "'",
                     genericCall.getLocation());
  }

  // Instantiate the generic class to get init method parameters
  std::vector<sun::TypePtr> expectedParamTypes;
  auto specializedClass =
      generics_.instantiateGenericClass(lookupName, typeArgs);
  if (specializedClass) {
    if (auto* initMethod = ctx_.accessibleMethod(*specializedClass, "init",
                                                 genericCall.getLocation())) {
      expectedParamTypes = initMethod->paramTypes;
    }
  }

  hintArrayLiteralArguments(args, expectedParamTypes);
  for (const auto& arg : args) {
    sema_.analyzeExpr(const_cast<ExprAST&>(*arg));
  }

  // Pick the init overload from the argument types, exactly as for a
  // non-generic class. Without this, a call with the wrong argument count
  // would silently skip the constructor in codegen.
  if (specializedClass) {
    if (auto params =
            resolveConstructorParams(*specializedClass, resolvedTypesOf(args),
                                     genericCall.getLocation())) {
      expectedParamTypes = std::move(*params);
    }
  }

  // Coerce integer literals to the init method's parameter types
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                            expectedParamTypes[i], /*throwOnFail=*/true);
  }

  if (specializedClass) {
    std::string displayName = specializedClass->toDisplayString() + ".init";
    sema_.checkArgumentPlaces(args, expectedParamTypes, displayName,
                              genericCall.getLocation());
    genericCall.setArgConversions(sun::conversions::classifyArguments(
        resolvedTypesOf(args), expectedParamTypes, /*cVariadic=*/false,
        displayName, genericCall.getLocation()));
  }

  genericCall.setResolvedType(types_.inferGenericCallType(genericCall));
}
