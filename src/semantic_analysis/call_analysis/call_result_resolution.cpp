/** Resolves call results and validates builtin call contracts. */
#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

using sun::support::logAndThrowError;
/** Resolves calls using the active semantic session. */
namespace sun::semantic_analysis {
using sun::types::EnumType;
using sun::types::StaticPointerType;
using sun::types::TypePtr;
using sun::types::Types;
using sun::types::unwrapRef;

// -------------------------------------------------------------------
// Intrinsic call result resolution
// -------------------------------------------------------------------

TypePtr CallAnalyzer::resolveIntrinsicCallType(
    const GenericCallAST& genericCall) {
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  const std::string& funcName = genericCall.getFunctionName();

  if (funcName == "_sizeof") {
    return Types::Int64();
  }
  if (funcName == "_load") {
    return typeArgs.empty() ? nullptr : typeArgs[0];
  }
  if (funcName == "_store" || funcName == "_init") {
    return Types::Void();
  }
  if (funcName == "_ptr_as_raw" || funcName == "_address_of") {
    return typeArgs.empty() ? nullptr : Types::RawPointer(typeArgs[0]);
  }
  if (funcName == "_to_ref") {
    return typeArgs.empty() ? nullptr : Types::Reference(typeArgs[0]);
  }
  if (funcName == "_is") {
    return Types::Bool();
  }
  if (funcName == "_deinit") {
    return Types::Void();
  }
  if (funcName == "_enum_from_int") {
    if (typeArgs.size() != 1 ||
        (!typeArgs[0]->isTypeParameter() &&
         (!typeArgs[0]->isEnum() ||
          static_cast<EnumType*>(typeArgs[0].get())->hasPayload()))) {
      logAndThrowError("_enum_from_int<T>: T must be an enum without payloads",
                       genericCall.getLocation());
    }
    const auto& args = genericCall.getArgs();
    if (args.size() != 1 || !args[0]->getResolvedType() ||
        (!sun::types::unwrapRef(args[0]->getResolvedType())->isIntegral() &&
         !sun::types::unwrapRef(args[0]->getResolvedType())
              ->isTypeParameter())) {
      logAndThrowError(
          "_enum_from_int<T> requires exactly one integer argument",
          genericCall.getLocation());
    }
    auto result = generics_.instantiateGenericEnum("std.Option", {typeArgs[0]});
    if (!result) {
      logAndThrowError("_enum_from_int<T> requires the standard library",
                       genericCall.getLocation());
    }
    return result;
  }
  if (funcName == "_convert" || funcName == "_bitcast") {
    return typeArgs.empty() ? nullptr : typeArgs[0];
  }
  // _spawn hands back the context it allocated. The type is the stdlib's own
  // ThreadContext, which only exists when std.thread has been loaded — and
  // _spawn is only ever written inside std.thread, so it always has.
  if (funcName == "_spawn") {
    auto context = ctx_.scope()->lookupClass("std.thread.ThreadContext");
    if (!context) {
      logAndThrowError(
          "_spawn requires the standard library's std.thread module",
          genericCall.getLocation());
    }
    return Types::RawPointer(context);
  }
  if (funcName == "_thread_join") {
    return typeArgs.empty() ? nullptr : typeArgs[0];
  }
  if (funcName == "_thread_join_drop") {
    return Types::Void();
  }

  logAndThrowError("Unknown intrinsic: '" + funcName + "'",
                   genericCall.getLocation());
}

// -------------------------------------------------------------------
// static_ptr<T> builtin methods
// -------------------------------------------------------------------

StaticPointerType* CallAnalyzer::asNonClassStaticPtr(const TypePtr& type) {
  if (!type || !type->isStaticPointer()) return nullptr;
  auto* staticPtr = static_cast<StaticPointerType*>(type.get());
  const auto& pointee = staticPtr->getPointeeType();
  if (pointee && pointee->isClass()) return nullptr;
  return staticPtr;
}

bool CallAnalyzer::isStaticPtrMethod(const std::string& name) {
  return name == "length" || name == "raw";
}

bool CallAnalyzer::isArrayMethod(const std::string& name) {
  return name == "ndims" || name == "dim";
}

TypePtr CallAnalyzer::resolveArrayMethodType(
    const std::string& name, const std::vector<TypePtr>& argTypes,
    const sun::support::Position& loc) {
  if (!isArrayMethod(name)) {
    logAndThrowError(
        "Array has no method '" + name + "'; available: ndims(), dim(i)", loc);
  }
  if (name == "ndims") {
    if (!argTypes.empty()) {
      logAndThrowError("array.ndims() takes no arguments", loc);
    }
    return Types::Int64();
  }
  if (argTypes.size() != 1 || !argTypes[0] ||
      !unwrapRef(argTypes[0])->isIntegral()) {
    logAndThrowError("array.dim(i) takes one integer argument", loc);
  }
  return Types::Int64();
}

TypePtr CallAnalyzer::resolveStaticPtrMethodType(
    const StaticPointerType& ptrType, const std::string& name, size_t argCount,
    const sun::support::Position& loc) {
  if (!isStaticPtrMethod(name)) {
    logAndThrowError(
        "static_ptr has no method '" + name + "'; available: length(), raw()",
        loc);
  }
  if (argCount != 0) {
    logAndThrowError("static_ptr." + name + "() takes no arguments", loc);
  }
  if (name == "length") return Types::Int64();
  return Types::RawPointer(ptrType.getPointeeType());
}

// -------------------------------------------------------------------
// Generic function result resolution
// -------------------------------------------------------------------

TypePtr CallAnalyzer::resolveGenericFunctionCallType(
    const GenericCallAST& genericCall) {
  if (auto calleeType = genericCall.getResolvedCalleeType()) {
    return static_cast<const sun::types::FunctionType&>(*calleeType)
        .getReturnType();
  }
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  const std::string& funcName = genericCall.getFunctionName();
  QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genFuncInfo = ctx_.lookupGenericFunction(lookupName);
  if (!genFuncInfo) {
    logAndThrowError("Unknown generic function: '" + funcName + "'",
                     genericCall.getLocation());
  }

  // No specialization - type args contain type parameters
  // Compute return type from generic function's declared return type
  if (genFuncInfo->returnType.has_value()) {
    SemanticContext::ScopeSwitchGuard definitionScope(
        ctx_, SemanticContext::definitionScopeOf(*genFuncInfo));
    SemanticContext::SourceFileGuard definitionFile(
        ctx_, genFuncInfo->AST->getSourceFileId());
    auto typeParams = sun::ast::typeParameterNames(genFuncInfo->typeParameters);
    ctx_.enterTypeParamScope(typeParams, typeArgs);
    TypePtr returnType =
        resolver_.typeAnnotationToType(*genFuncInfo->returnType);
    ctx_.exitScope();
    return returnType;
  }

  // No declared return type - can't infer without instantiation
  logAndThrowError("Generic function '" + funcName +
                       "' called with unresolved type parameters requires a "
                       "declared return type",
                   genericCall.getLocation());
}

}  // namespace sun::semantic_analysis
