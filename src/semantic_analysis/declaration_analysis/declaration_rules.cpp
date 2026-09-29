/** Checks a declaration on its own, without looking anything up in scope. */
#include "semantic_analysis/declaration_analysis/declaration_rules.h"

#include "ast.h"
#include "codegen/abi/c_abi_types.h"
#include "codegen/support/type_checks.h"
#include "semantic_analysis/symbol_names.h"

using sun::ast::FunctionAST;
using sun::ast::PrototypeAST;
using sun::support::logAndThrowError;
using sun::support::Position;
using sun::types::TypePtr;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

void validateNotReserved(const std::string& name, const std::string& kind,
                         std::optional<Position> location) {
  if (isReservedIdentifier(name)) {
    logAndThrowError(kind + " '" + name +
                         "' is invalid: names starting with '_' are "
                         "reserved for builtins",
                     location);
  }
}

void validateExternSignature(const FunctionAST& func) {
  const PrototypeAST& proto = func.getProto();

  // C varargs only make sense at a C boundary — a Sun function body has no
  // way to read them (no va_arg), so allowing `...` there would compile to a
  // signature nothing can use.
  if (proto.hasVariadicParam()) {
    logAndThrowError("Extern function '" + proto.getName() +
                         "' cannot use a named variadic pack; use C varargs "
                         "('...') instead",
                     func.getLocation());
  }

  auto describe = [](const TypePtr& t) {
    return t ? t->toDisplayString() : std::string("<unresolved>");
  };

  auto validateCallback = [&](const sun::types::FunctionType& callback,
                              const std::string& paramName) {
    if (callback.canThrow()) {
      logAndThrowError("C callback parameter '" + paramName + "' cannot throw",
                       func.getLocation());
    }
    for (const auto& callbackParam : callback.getParamTypes()) {
      if (!sun::codegen::abi::isCallbackParameter(callbackParam)) {
        logAndThrowError("C callback parameter '" + paramName +
                             "' has unsupported callback argument type '" +
                             describe(callbackParam) + "'",
                         func.getLocation());
      }
    }
    if (!sun::codegen::abi::isCallbackReturn(callback.getReturnType())) {
      logAndThrowError("C callback parameter '" + paramName +
                           "' has unsupported callback return type '" +
                           describe(callback.getReturnType()) + "'",
                       func.getLocation());
    }
  };

  if (proto.hasResolvedParamTypes()) {
    const auto& params = proto.getResolvedParamTypes();
    for (size_t i = 0; i < params.size(); ++i) {
      if (params[i] && params[i]->isVoid()) {
        logAndThrowError("Parameter '" + proto.getArgs()[i].first +
                             "' of extern function '" + proto.getName() +
                             "' cannot be void",
                         func.getLocation());
      }
      if (auto* callback =
              sun::codegen::support::tryGetType<sun::types::FunctionType>(
                  params[i])) {
        validateCallback(*callback, proto.getArgs()[i].first);
      }
      if (!sun::codegen::abi::isValue(params[i])) {
        logAndThrowError(
            "Parameter '" + proto.getArgs()[i].first +
                "' of extern function '" + proto.getName() + "' has type '" +
                describe(params[i]) +
                "', which has no C equivalent. Extern parameters must be a "
                "primitive, an enum, raw_ptr<T>, ref T (which is C's T*), or "
                "a class or function pointer.",
            func.getLocation());
      }
    }
  }

  if (proto.hasResolvedReturnType() &&
      !sun::codegen::abi::isReturn(proto.getResolvedReturnType())) {
    logAndThrowError(
        "Extern function '" + proto.getName() + "' returns '" +
            describe(proto.getResolvedReturnType()) +
            "', which has no C equivalent. Extern return types must be a "
            "primitive, an enum, raw_ptr<T>, or a class. Note that `ref T` "
            "cannot be returned; use raw_ptr<T>.",
        func.getLocation());
  }
}

void rejectRefEnvReturnType(
    const std::optional<sun::ast::TypeAnnotation>& returnType,
    const Position& location, bool allowNamed) {
  if (returnType && returnType->refEnv) {
    if (!returnType->isAnonymousFrameLambda()) {
      if (allowNamed) return;
      logAndThrowError(
          "a named frame-bound return type requires a lifetime available "
          "in this declaration",
          location);
    }
    logAndThrowError(
        "an anonymous <'_> lambda type cannot be a return type - its captured "
        "environment lives in a stack frame that dies when the function "
        "returns. Name the frame with a lifetime to allow it: "
        "function f<'a>(x: <'a>() => i32) <'a>() => i32",
        location);
  }
}

void rejectDuplicateLifetimes(
    const std::vector<sun::ast::LifetimeParameter>& params,
    const std::string& owner) {
  for (size_t i = 0; i < params.size(); ++i) {
    for (size_t j = i + 1; j < params.size(); ++j) {
      if (params[j].name == params[i].name) {
        logAndThrowError(
            "duplicate lifetime parameter '" + params[i].name + owner,
            params[i].span);
      }
    }
  }
}

}  // namespace sun::semantic_analysis
