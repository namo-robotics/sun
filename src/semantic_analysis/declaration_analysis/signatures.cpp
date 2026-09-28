/** Validates declared names, signatures, and lifetime annotations. */
#include <algorithm>
#include <cassert>
#include <set>

#include "codegen/abi/c_abi_types.h"
#include "semantic_analysis/class_analysis/field_initialization.h"
#include "semantic_analysis/expression_analysis/expression_properties.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_analysis/type_rules.h"
#include "semantic_analysis/visibility.h"
#include "support/config.h"
#include "support/error.h"

using sun::types::ClassMethod;
using sun::types::LambdaType;
using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::IndexAST;
using sun::ast::MemberAccessAST;
using sun::ast::MemberAssignmentAST;
using sun::ast::PrototypeAST;
using sun::ast::TernaryExprAST;
using sun::support::logAndThrowError;
using sun::support::Position;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::types::Type;

using sun::semantic_analysis::isBorrowableLvalue;
using sun::semantic_analysis::isReservedIdentifier;
using sun::semantic_analysis::methodVisibility;
using sun::semantic_analysis::type_analysis::isAssignableTo;
using sun::semantic_analysis::type_analysis::tryCoerceIntegerLiteral;
using sun::types::unwrapRef;

// -------------------------------------------------------------------
// Signature validation
// -------------------------------------------------------------------

std::vector<TypePtr> DeclarationAnalyzer::validateAndResolveParamTypes(
    PrototypeAST& proto, std::optional<Position> loc,
    bool allowByValueObjects) {
  // Validate parameter names
  for (const auto& argName : proto.getArgNames()) {
    validateNotReserved(argName, "Parameter name", loc);
  }

  // Resolve parameter types
  std::vector<TypePtr> paramTypes;
  for (auto& [argName, argType] : proto.getMutableArgs()) {
    TypePtr paramType = sema_.typeResolver().typeAnnotationToType(argType);

    /**
     * Check for compound types being passed by value
     */
    if constexpr (sun::support::Config::REQUIRE_REF_FOR_COMPOUND_PARAMS) {
      // C externs are exempt: passing a struct by value is what the C ABI
      // specifies, so it is the callee's signature rather than a Sun choice.
      if (!allowByValueObjects && paramType && paramType->isCompound()) {
        // Error: compound types must be passed by reference
        logAndThrowError("Parameter '" + argName + "' has compound type '" +
                             paramType->toDisplayString() +
                             "' which cannot be passed by value. Use 'ref " +
                             paramType->toDisplayString() + "' instead.",
                         loc);
      }
    }
    // When REQUIRE_REF_FOR_COMPOUND_PARAMS is false, compound types are
    // passed by value with move semantics - no ref wrapping needed.

    paramTypes.push_back(paramType);
  }

  return paramTypes;
}

FunctionInfo DeclarationAnalyzer::getFunctionInfo(FunctionAST& func,
                                                  bool allowInterfaceReturn) {
  SemanticContext::SourceFileGuard sourceFile(ctx_, func.getSourceFileId());
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Only declared generic parameters may remain unresolved in a signature.
  SemanticContext::ScopeSwitchGuard signatureScope(ctx_, ctx_.scope());
  if (!proto.getTypeParameters().empty()) {
    std::vector<TypePtr> parameters;
    for (size_t i = 0; i < proto.getTypeParameters().size(); ++i) {
      parameters.push_back(proto.getTypeParameters()[i].toSunType(
          ctx_.results().declarations,
          proto.declarationIdentity().typeParameters.at(i)));
    }
    ctx_.enterTypeParamScope(proto.getTypeParameterNames(), parameters);
  }

  // Validate function name (if named function, not lambda)
  if (!proto.getName().empty()) {
    validateNotReserved(proto.getName(), "Function name", func.getLocation());
  }

  std::vector<sun::ast::Capture> captures;

  // Validate and resolve parameter types. Only C externs may take objects by
  // value; see validateAndResolveParamTypes.
  std::vector<TypePtr> paramTypes = validateAndResolveParamTypes(
      proto, func.getLocation(), /*allowByValueObjects=*/func.isCExtern());

  // Resolve return type if specified; Void for constructors (no return type)
  TypePtr returnType = Types::Void();
  if (proto.hasReturnType()) {
    returnType = sema_.typeResolver().typeAnnotationToType(
        *proto.getReturnType(), allowInterfaceReturn);
    if (!returnType) {
      logAndThrowError("Failed to resolve return type for function '" +
                           proto.getName() + "'",
                       func.getLocation());
    }
  }

  assert(proto.hasQualifiedName() &&
         "Function declaration must be named first");
  const auto& qualifiedName = proto.getQualifiedName();

  FunctionInfo info;
  info.returnType = returnType;
  info.paramTypes = std::move(paramTypes);
  info.captures = std::move(captures);
  info.qualifiedName = qualifiedName;
  info.declarationId = proto.getDeclarationId();
  info.canThrow = proto.canThrow();
  info.isCVariadic = proto.isCVariadic();
  info.isCExtern = func.isCExtern();
  info.isForwardDeclaration =
      func.isExtern() && !func.isCExtern() && !func.isPrecompiled();
  info.visibility = func.getVisibility();
  return info;
}

void DeclarationAnalyzer::applyFunctionInfoToProto(PrototypeAST& proto,
                                                   const FunctionInfo& info) {
  proto.setQualifiedName(info.qualifiedName);
  proto.setCaptures(info.captures);
  proto.setResolvedParamTypes(info.paramTypes);
  proto.setResolvedReturnType(info.returnType);
}

void DeclarationAnalyzer::validateNotReserved(
    const std::string& name, const std::string& kind,
    std::optional<Position> location) {
  if (isReservedIdentifier(name)) {
    logAndThrowError(kind + " '" + name +
                         "' is invalid: names starting with '_' are "
                         "reserved for builtins",
                     location);
  }
}

void DeclarationAnalyzer::validateExternSignature(FunctionAST& func) {
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

void DeclarationAnalyzer::rejectRefEnvReturnType(
    const std::optional<sun::ast::TypeAnnotation>& returnType,
    const Position& location, bool allowNamed) {
  if (returnType && returnType->refEnv) {
    if (allowNamed && !returnType->lifetimeName.empty() &&
        returnType->lifetimeName != "_") {
      return;
    }
    if (!returnType->lifetimeName.empty() && returnType->lifetimeName != "_") {
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

void DeclarationAnalyzer::checkAnnotationLifetimes(
    const sun::ast::TypeAnnotation& annot, const Position& location) {
  auto checkName = [&](const std::string& name) {
    if (name == "_") return;
    if (name == "this") {
      if (!ctx_.allowsThisLifetime()) {
        logAndThrowError(
            "the 'this lifetime is only usable inside class and interface "
            "members - it names the receiver's lifetime",
            location);
      }
      return;
    }
    if (std::find(ctx_.activeLifetimeNames().begin(),
                  ctx_.activeLifetimeNames().end(),
                  name) == ctx_.activeLifetimeNames().end()) {
      logAndThrowError("use of undeclared lifetime '" + name +
                           ". Declare it on a function (function f<'" + name +
                           ">) or lambda literal (<'" + name +
                           ">(...) => ...), or on the class (class C<'" + name +
                           ">)",
                       location);
    }
  };
  if (!annot.lifetimeName.empty()) checkName(annot.lifetimeName);
  for (const auto& name : annot.lifetimeArguments) checkName(name);
  if (annot.elementType) checkAnnotationLifetimes(*annot.elementType, location);
  for (const auto& param : annot.paramTypes) {
    checkAnnotationLifetimes(*param, location);
  }
  if (annot.returnType) checkAnnotationLifetimes(*annot.returnType, location);
  for (const auto& typeArg : annot.typeArguments) {
    checkAnnotationLifetimes(*typeArg, location);
  }
}

void DeclarationAnalyzer::checkSignatureLifetimes(const PrototypeAST& proto,
                                                  const Position& location) {
  for (const auto& lp : proto.getLifetimeParameters()) {
    if (std::count_if(proto.getLifetimeParameters().begin(),
                      proto.getLifetimeParameters().end(),
                      [&](const sun::ast::LifetimeParameter& other) {
                        return other.name == lp.name;
                      }) > 1) {
      logAndThrowError("duplicate lifetime parameter '" + lp.name, lp.span);
    }
    if (std::find(ctx_.activeLifetimeNames().begin(),
                  ctx_.activeLifetimeNames().end(),
                  lp.name) != ctx_.activeLifetimeNames().end()) {
      logAndThrowError("lifetime '" + lp.name +
                           " is already declared by an enclosing declaration",
                       lp.span);
    }
  }
  SemanticContext::LifetimeScopeGuard lifetimes(ctx_);
  for (const auto& lp : proto.getLifetimeParameters()) {
    ctx_.declareLifetime(lp.name);
  }
  for (const auto& [argName, argType] : proto.getArgs()) {
    checkAnnotationLifetimes(argType, location);
  }
  if (proto.hasReturnType()) {
    checkAnnotationLifetimes(*proto.getReturnType(), location);
  }
}

FunctionInfo DeclarationAnalyzer::getLambdaInfo(sun::ast::LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Build captures using current scope information
  std::vector<sun::ast::Capture> captures = buildCaptures(lambda);

  // Validate and resolve parameter types
  std::vector<TypePtr> paramTypes = validateAndResolveParamTypes(proto);

  // Resolve return type (Sun requires return type annotations on lambdas,
  // parser enforces this, but check defensively)
  TypePtr returnType = Types::Void();
  if (proto.hasReturnType()) {
    returnType =
        sema_.typeResolver().typeAnnotationToType(*proto.getReturnType());
    if (!returnType) {
      logAndThrowError("Failed to resolve return type for lambda",
                       lambda.getLocation());
    }
  }

  return {returnType, paramTypes, captures};
}

void DeclarationAnalyzer::validateTypeParameter(const TypePtr& type,
                                                const ExprAST& node) {
  if (!type || !type->isTypeParameter()) return;

  auto* typeParam =
      static_cast<const sun::types::TypeParameterType*>(type.get());

  // Type traits (_Integer, _Float, etc.) are not scope-bound type parameters
  if (sun::semantic_analysis::type_analysis::isTypeTrait(typeParam->getName()))
    return;

  TypePtr found = ctx_.findTypeParameter(typeParam->getName());
  if (!found) {
    const Position& loc = node.getLocation();
    std::string msg = "Unknown type parameter '" + typeParam->getName() +
                      "' at " + std::to_string(loc.line) + ":" +
                      std::to_string(loc.column) + " in '" + node.toString() +
                      "'. This is a bug in the compiler - please report it.";
    logAndThrowError(msg, loc);
  }
}

}  // namespace sun::semantic_analysis
