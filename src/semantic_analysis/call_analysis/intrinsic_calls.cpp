/** Resolves compiler intrinsics without putting their special rules in ordinary
 * call analysis. */
#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/call_analysis/argument_conversion.h"
#include "semantic_analysis/call_analysis/call_analyzer.h"
#include "semantic_analysis/generic_analysis/generic_type_arguments.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_analysis/type_inferer.h"
#include "semantic_analysis/type_analysis/type_rules.h"

/** Checks intrinsic contracts and syntax-dependent compiler operations. */
namespace sun::semantic_analysis {
using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::ast::VariableReferenceAST;
using sun::semantic_analysis::type_analysis::tryCoerceIntegerLiteral;
using sun::support::logAndThrowError;
using sun::types::FunctionType;
using sun::types::TypePtr;
using sun::types::Types;
using sun::types::unwrapRef;

using sun::semantic_analysis::type_analysis::isAssignableTo;
using sun::types::ClassMethod;
using sun::types::ClassType;
using sun::types::LambdaType;

/** Keeps intrinsic argument collection local to this implementation. */
namespace {
/** Collects analyzed argument types for signature checking and ownership
 * conversions. */
std::vector<TypePtr> resolvedTypesOf(
    const std::vector<std::unique_ptr<ExprAST>>& arguments) {
  std::vector<TypePtr> types;
  for (const auto& argument : arguments) {
    types.push_back(argument->getResolvedType());
  }
  return types;
}
}  // namespace

bool CallAnalyzer::analyzeSpecialIntrinsicCall(CallExprAST& callExpr) {
  const auto* callee =
      dynamic_cast<const VariableReferenceAST*>(callExpr.getCallee());
  if (!callee) {
    return false;
  }
  const auto& name = callee->getName();
  checkRequiresUnsafeBlock(name, callExpr.getLocation());
  if (name == "_assert_c_target" &&
      (!ctx_.isAtModuleLevel() || callExpr.getArgs().size() != 1 ||
       callExpr.getArgs()[0]->getType() != ASTNodeType::STRING_LITERAL)) {
    logAndThrowError(
        "_assert_c_target requires one string literal at module scope",
        callExpr.getLocation());
  }
  const bool userDefined = ctx_.currentScope().lookupVariable(name) ||
                           !ctx_.currentScope().getAllFunctions(name).empty() ||
                           ctx_.currentScope().lookupGenericFunction(name);
  if (userDefined || name != "_division_overflows") {
    return false;
  }
  if (callExpr.getArgs().size() != 2) {
    logAndThrowError(name + " expects two integer operands",
                     callExpr.getLocation());
  }
  auto& left = *callExpr.getArgs()[0];
  auto& right = *callExpr.getArgs()[1];
  sema_.analyzeExpr(left);
  sema_.analyzeExpr(right);
  auto leftType = unwrapRef(left.getResolvedType());
  auto rightType = unwrapRef(right.getResolvedType());
  if (leftType->isIntegral()) {
    tryCoerceIntegerLiteral(&right, leftType, false);
  }
  if (rightType->isIntegral()) {
    tryCoerceIntegerLiteral(&left, rightType, false);
  }
  leftType = unwrapRef(left.getResolvedType());
  rightType = unwrapRef(right.getResolvedType());
  if ((!leftType->isIntegral() && !leftType->isTypeParameter()) ||
      (!rightType->isIntegral() && !rightType->isTypeParameter())) {
    logAndThrowError(name + " requires integer operands",
                     callExpr.getLocation());
  }
  auto numeric = sun::semantic_analysis::type_analysis::TypeInferer::numeric(
      leftType, rightType);
  auto valueType = std::get_if<TypePtr>(&numeric);
  if (!valueType) {
    logAndThrowError("Incompatible checked arithmetic operands",
                     callExpr.getLocation());
  }
  auto result = Types::Bool();
  const_cast<ExprAST*>(callExpr.getCallee())
      ->setResolvedType(std::make_shared<FunctionType>(
          result, std::vector<TypePtr>{*valueType, *valueType}));
  callExpr.setResolvedType(result);
  callExpr.setArgConversions(
      {sun::semantic_analysis::ArgConversion::PassValue,
       sun::semantic_analysis::ArgConversion::PassValue});
  return true;
}

TypePtr CallAnalyzer::resolveDeclaredIntrinsicSignature(
    const GenericFunctionInfo& intrinsic, const std::vector<TypePtr>& arguments,
    const std::vector<TypePtr>& explicitTypes,
    std::optional<sun::support::Position> location) {
  const auto& declaration = *intrinsic.intrinsicPrototype;
  const auto fixed = declaration.getArgs().size();
  if ((!declaration.hasVariadicParam() && arguments.size() != fixed) ||
      arguments.size() < fixed) {
    logAndThrowError("Function '" + declaration.getName() + "' expects " +
                         (declaration.hasVariadicParam() ? "at least " : "") +
                         std::to_string(fixed) + " arguments, got " +
                         std::to_string(arguments.size()),
                     location);
  }
  // These operations still have semantic rules beyond a substituted signature.
  if (sun::codegen::intrinsics::isIntrinsic(declaration.getName())) {
    logAndThrowError("Intrinsic '" + declaration.getName() +
                         "' requires explicit type arguments",
                     location);
  }
  auto types = resolveGenericTypeArguments(
      intrinsic, arguments, declaration.getName(), location, explicitTypes);
  return generics_.genericFunctionSignature(intrinsic, types);
}

void CallAnalyzer::analyzeDeclaredIntrinsicCall(
    GenericCallAST& call, const GenericFunctionInfo& intrinsic) {
  std::vector<TypePtr> arguments;
  for (const auto& argument : call.getArgs()) {
    sema_.analyzeExpr(*argument);
    arguments.push_back(argument->getResolvedType());
  }
  auto signature = resolveDeclaredIntrinsicSignature(
      intrinsic, arguments, call.getResolvedTypeArgs(), call.getLocation());
  const auto& function = static_cast<const FunctionType&>(*signature);
  sema_.expressions().checkArgumentPlaces(
      call.getArgs(), function.getParamTypes(), call.getFunctionName(),
      call.getLocation());
  call.setArgConversions(classifyArguments(arguments, function.getParamTypes(),
                                           false, call.getFunctionName(),
                                           call.getLocation()));
  call.setResolvedCalleeType(signature);
  call.setResolvedType(function.getReturnType());
}

void CallAnalyzer::analyzeIntrinsicCall(GenericCallAST& genericCall) {
  // Intrinsics are handled at codegen time - just analyze arguments
  for (const auto& arg : genericCall.getArgs()) {
    sema_.analyzeExpr(const_cast<ExprAST&>(*arg));
  }
  // Expand any variadic pack into concrete typed args (e.g. _init<T>(p,
  // args...))
  expandPackArguments(genericCall.getArgsMutable());
  genericCall.setResolvedType(resolveIntrinsicCallType(genericCall));
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
// class constructor, codegen reports unexpected arguments. Non-class values
// accept either no argument for zero initialization or one value of the target
// type.
void CallAnalyzer::recordInitArgumentConversions(GenericCallAST& genericCall) {
  const auto& args = genericCall.getArgs();
  if (args.empty()) {
    return;  // codegen reports the missing pointer
  }

  std::vector<TypePtr> argTypes = resolvedTypesOf(args);
  std::vector<TypePtr> ctorArgTypes(argTypes.begin() + 1, argTypes.end());

  const ClassMethod* init = nullptr;
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  if (!typeArgs.empty() && typeArgs[0] && typeArgs[0]->isClass()) {
    init = static_cast<const ClassType&>(*typeArgs[0])
               .getMethodForArgs("init", ctorArgTypes);
  }

  if (!typeArgs.empty() && typeArgs[0] && !typeArgs[0]->isClass() &&
      !typeArgs[0]->isTypeParameter()) {
    auto targetType = typeArgs[0];
    if (ctorArgTypes.size() > 1) {
      logAndThrowError("_init<T> for a non-class type takes at most one value",
                       genericCall.getLocation());
    }
    if (!ctorArgTypes.empty() && !isAssignableTo(ctorArgTypes[0], targetType)) {
      logAndThrowError("_init<T> value must be assignable to " +
                           targetType->toDisplayString(),
                       genericCall.getLocation());
    }
    std::vector<TypePtr> paramTypes{argTypes[0], targetType};
    genericCall.setArgConversions(sun::semantic_analysis::classifyArguments(
        argTypes, paramTypes, /*cVariadic=*/false, "_init",
        genericCall.getLocation()));
    return;
  }

  if (init) {
    const auto* function = ctx_.currentFunctionScope();
    if (init->canThrow && !(function && function->functionCanThrow) &&
        !ctx_.currentScope().hasExceptionHandler()) {
      logAndThrowError(
          "Initializing a throwing constructor requires a try block or a "
          "throws declaration",
          genericCall.getLocation());
    }
    genericCall.setTargetDeclarationId(init->declarationId);
    genericCall.setResolvedType(init->returnType);
  }
  std::vector<TypePtr> paramTypes{argTypes[0]};
  for (size_t i = 0; i < ctorArgTypes.size(); ++i) {
    paramTypes.push_back(init ? init->paramTypes[i] : ctorArgTypes[i]);
  }
  genericCall.setArgConversions(sun::semantic_analysis::classifyArguments(
      argTypes, paramTypes, /*cVariadic=*/false, "_init",
      genericCall.getLocation()));
}

// The callee is argument 0 and is taken apart rather than passed on, so it
// stands in for itself; everything after it fills the callee's parameters.
// F may be a lambda or a named-function value.
void CallAnalyzer::recordSpawnArgumentConversions(GenericCallAST& genericCall) {
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  auto* lambda =
      typeArgs.empty()
          ? nullptr
          : sun::codegen::support::tryGetType<LambdaType>(typeArgs[0]);
  auto* namedFn =
      typeArgs.empty()
          ? nullptr
          : sun::codegen::support::tryGetType<sun::types::FunctionType>(
                typeArgs[0]);
  if (!lambda && !namedFn) {
    logAndThrowError("_spawn<F> requires a lambda or function type argument",
                     genericCall.getLocation());
  }

  // The trampoline that runs the thread has no unwind handling, so an error
  // escaping the spawned function would take the process down mid-unwind.
  if (lambda ? lambda->requiresUnsafe() : namedFn->requiresUnsafe()) {
    logAndThrowError(
        "a spawned function must be safe to call; wrap the unsafe call in a "
        "lambda",
        genericCall.getLocation());
  }
  if (lambda ? lambda->canThrow() : namedFn->canThrow()) {
    logAndThrowError(
        "a spawned function must not throw; catch errors inside it and "
        "return them as part of its result",
        genericCall.getLocation());
  }

  const auto& args = genericCall.getArgs();
  std::vector<TypePtr> paramTypes{typeArgs[0]};
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
    sema_.expressions().checkMoveSource(*arg, genericCall.getLocation());
  }
  genericCall.setArgConversions(sun::semantic_analysis::classifyArguments(
      resolvedTypesOf(args), paramTypes, /*cVariadic=*/false, "_spawn",
      genericCall.getLocation()));
}

}  // namespace sun::semantic_analysis
