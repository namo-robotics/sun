#include "semantic_analysis/body_analyzer.h"

#include "ast.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_rules.h"
#include "support/error.h"

using sun::semantic_analysis::TypePtr;

using sun::ast::BlockExprAST;
using sun::ast::PrototypeAST;
using sun::support::logAndThrowError;

namespace sun::semantic_analysis {

void BodyAnalyzer::analyzeBlock(BlockExprAST& block) {
  for (const auto& expression : block.getBody()) {
    analyzer_.analyzeExpr(*expression);
  }
}

// Sun has no implicit returns: a function whose signature promises a value
// must leave through an explicit `return` (or a throw) on every path. Checked
// after the body is analyzed, so match discriminants carry their types.
static void checkAllPathsReturn(const PrototypeAST& proto,
                                const BlockExprAST& body,
                                const TypePtr& returnType,
                                const sun::support::Position& loc) {
  if (!returnType || returnType->isVoid()) return;
  if (sun::semantic_analysis::alwaysExits(body)) return;
  const std::string name =
      proto.getName().empty() ? "lambda" : "'" + proto.getName() + "'";
  logAndThrowError(
      "Function " + name + " can reach the end of its body without a value: " +
          "it must end in a `return` (or a throw) on every path. Sun has no "
          "implicit returns.",
      loc);
}

void BodyAnalyzer::analyzeFunction(sun::ast::FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  analyzer_.rejectRefEnvReturnType(proto.getReturnType(), func.getLocation(),
                                   /*allowNamed=*/true);

  // Lifetime names in the signature must be declared; 'this needs a class
  bool savedAllowThis = analyzer_.allowThisLifetime_;
  analyzer_.allowThisLifetime_ = ctx_.getCurrentClass() != nullptr;
  analyzer_.checkSignatureLifetimes(proto, func.getLocation());
  analyzer_.allowThisLifetime_ = savedAllowThis;

  // For extern functions (no body), just validate and return
  if (func.isExtern()) {
    if (!proto.hasReturnType()) {
      logAndThrowError("Extern function '" + proto.getName() +
                           "' must have an explicit return type",
                       func.getLocation());
    }
    if (func.isCExtern()) analyzer_.validateExternSignature(func);
    return;
  }

  // A pack's arity and types come from the call site, so a template body
  // holding one has nothing concrete to check yet. Each specialization is
  // analyzed on instantiation (instantiateGenericFunction/Method).
  if (proto.hasVariadicParam()) return;

  // Sun has no va_arg, so C varargs are only meaningful on an extern
  // declaration where the callee is C code.
  if (proto.isCVariadic()) {
    logAndThrowError(
        "C varargs ('...') are only allowed on 'extern function' "
        "declarations; '" +
            proto.getName() + "' has a body",
        func.getLocation());
  }

  // Format the function signature for scope diagnostics.
  std::string funcSig = sun::semantic_analysis::formatFunctionSignature(
      proto.getQualifiedName().display(), proto.getResolvedParamTypes());

  // Return type for return-position inference. Some paths (class method
  // pass 2) reach here before the proto's resolved return type is applied;
  // resolve the annotation in the current scope (type parameter bindings for
  // specialized classes are active here).
  TypePtr scopeReturnType = proto.getResolvedReturnType();
  if (!scopeReturnType && proto.hasReturnType() && !proto.isGeneric()) {
    scopeReturnType =
        analyzer_.types().typeAnnotationToType(*proto.getReturnType());
  }

  // Enter the function scope with its diagnostic signature.
  // Pass canThrow flag so throw expressions can be validated. A const method
  // body sees the const view of its return type: borrows of `this` are
  // `const ref` there, and the declared `ref` result is what callers with a
  // mutable receiver get.
  if (proto.isConstMethod())
    scopeReturnType = analyzer_.types().createConstView(scopeReturnType);
  ctx_.enterFunctionScope(funcSig, proto.getQualifiedName(), proto.canThrow(),
                          scopeReturnType);

  // Declare 'this' for methods (when we're inside a class context); it is
  // immutable inside a const method
  if (ctx_.getCurrentClass()) {
    ctx_.currentScope().declareVariable("this", ctx_.getCurrentClass(),
                                        /*isParam=*/true,
                                        /*isConst=*/proto.isConstMethod());
  }

  // If this is a generic function/method, bind each type parameter to itself
  // so the body can be analyzed before any specialization exists. The binding
  // carries the parameter's constraint, which is what lets `<T: IShape>` reach
  // IShape's members on a value of type T (see inferMemberAccessType).
  if (proto.isGeneric()) {
    std::vector<std::string> typeParams;
    std::vector<TypePtr> typeParamTypes;
    for (size_t i = 0; i < proto.getTypeParameters().size(); ++i) {
      const auto& tp = proto.getTypeParameters()[i];
      typeParams.push_back(tp.name);
      typeParamTypes.push_back(
          tp.toSunType(ctx_.types()->declarations,
                       proto.declarationIdentity().typeParameters.at(i)));
    }
    ctx_.currentScope().declareTypeParameters(typeParams, typeParamTypes);
  }

  // Field defaults see the definition scope and this, before parameters exist.
  bool savedAllowThisForDefaults = analyzer_.allowThisLifetime_;
  analyzer_.allowThisLifetime_ = ctx_.getCurrentClass() != nullptr;
  const auto& statements = func.getBody().getBody();
  for (size_t i = 0; i < func.getFieldInitializerCount(); ++i) {
    analyzer_.analyzeExpr(*statements.at(i));
  }
  analyzer_.allowThisLifetime_ = savedAllowThisForDefaults;

  // Declare parameters
  for (size_t i = 0; i < proto.getArgs().size(); ++i) {
    const auto& [argName, argType] = proto.getArgs()[i];
    TypePtr paramType = analyzer_.types().typeAnnotationToType(argType);
    ctx_.currentScope().declareVariable(
        argName, paramType, true, false,
        proto.declarationIdentity().parameters.at(i));
  }

  // Add captured variables to scope (so nested functions can see them),
  // marked as captures so mutation checks and nested capture lists can
  // distinguish them from ordinary locals
  for (const auto& cap : proto.getCaptures()) {
    ctx_.currentScope().declareVariable(cap.name, cap.type, false, cap.isConst,
                                        cap.declarationId);
    if (VariableInfo* vi = ctx_.currentScope().lookupVariable(cap.name)) {
      vi->captureKind = cap.kind;
      vi->isConst = cap.isConst;
    }
  }

  // Analyze the function body. The signature's lifetime names stay active
  // so annotations inside the body (locals, lambdas) can use them.
  size_t lifetimeMark = analyzer_.activeLifetimeNames_.size();
  for (const auto& lp : proto.getLifetimeParameters()) {
    analyzer_.activeLifetimeNames_.push_back(lp.name);
  }
  bool savedAllowThisForBody = analyzer_.allowThisLifetime_;
  analyzer_.allowThisLifetime_ = ctx_.getCurrentClass() != nullptr;
  for (size_t i = func.getFieldInitializerCount(); i < statements.size(); ++i) {
    analyzer_.analyzeExpr(*statements[i]);
  }
  analyzer_.allowThisLifetime_ = savedAllowThisForBody;
  analyzer_.activeLifetimeNames_.resize(lifetimeMark);

  // No implicit returns: a non-void signature must be met by an explicit
  // return (or throw) on every path. Moon stubs carry no body to check.
  if (!ctx_.isInMoonScope()) {
    checkAllPathsReturn(proto, func.getBody(), scopeReturnType,
                        func.getLocation());
  }

  ctx_.exitScope();
}

void BodyAnalyzer::analyzeLambda(sun::ast::LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Enter function scope (empty signature - lambdas are anonymous)
  // Nested functions in lambdas will still get outer function prefixes
  // Pass canThrow flag from the lambda's prototype
  ctx_.enterFunctionScope("", sun::semantic_analysis::QualifiedName(),
                          proto.canThrow(), proto.getResolvedReturnType());

  // Lambdas don't have type parameters (no generic lambdas)

  // Declare parameters
  for (size_t i = 0; i < proto.getArgs().size(); ++i) {
    const auto& [argName, argType] = proto.getArgs()[i];
    TypePtr paramType = analyzer_.types().typeAnnotationToType(argType);
    ctx_.currentScope().declareVariable(
        argName, paramType, true, false,
        proto.declarationIdentity().parameters.at(i));
  }

  // Add captured variables to scope (so nested functions can see them),
  // marked as captures so mutation checks and nested capture lists can
  // distinguish them from ordinary locals
  for (const auto& cap : proto.getCaptures()) {
    ctx_.currentScope().declareVariable(cap.name, cap.type, false, cap.isConst,
                                        cap.declarationId);
    if (VariableInfo* vi = ctx_.currentScope().lookupVariable(cap.name)) {
      vi->captureKind = cap.kind;
      vi->isConst = cap.isConst;
    }
  }

  // Keep the lambda's lifetime binders active for annotations nested in
  // its body, just as a named function does.
  size_t lifetimeMark = analyzer_.activeLifetimeNames_.size();
  for (const auto& lp : proto.getLifetimeParameters()) {
    analyzer_.activeLifetimeNames_.push_back(lp.name);
  }
  analyzeBlock(const_cast<BlockExprAST&>(lambda.getBody()));
  analyzer_.activeLifetimeNames_.resize(lifetimeMark);

  // Same rule as named functions: no implicit returns
  checkAllPathsReturn(proto, lambda.getBody(), proto.getResolvedReturnType(),
                      lambda.getLocation());

  ctx_.exitScope();
}

// Analyze one (cloned) method body of a specialized class. The caller has
// entered the specialized class's scope inside the template's definition
// scope, so the body sees exactly the names the template was written against.
void BodyAnalyzer::analyzeMethodWithBindings(
    sun::ast::FunctionAST& methodFunc,
    std::shared_ptr<sun::semantic_analysis::ClassType> classType,
    const std::vector<std::string>& typeParams,
    const std::vector<TypePtr>& typeArgs) {
  SemanticContext::SourceFileGuard sourceFile(ctx_,
                                              methodFunc.getSourceFileId());
  // Step 2: Set up scope with type parameter bindings (only if needed)
  // For generic class methods, type bindings are already in the Class scope
  bool needsTypeParamScope =
      !typeParams.empty() && typeParams.size() == typeArgs.size();
  if (needsTypeParamScope) {
    ctx_.enterTypeParamScope(typeParams, typeArgs);
  }

  // Step 3: Set class context for 'this' member access resolution
  auto savedClass = ctx_.getCurrentClass();
  if (classType) {
    ctx_.setCurrentClass(classType);
  }

  // Step 4: Enter method scope and declare 'this' parameter
  // Compute method signature with substituted param types for nested function
  // qualification
  const auto& proto = methodFunc.getProto();
  std::vector<TypePtr> substitutedParamTypes;
  for (const auto& [argName, argType] : proto.getArgs()) {
    substitutedParamTypes.push_back(
        analyzer_.types().typeAnnotationToType(argType));
  }
  std::string methodSig = sun::semantic_analysis::formatFunctionSignature(
      proto.getName(), substitutedParamTypes);
  // Resolve the return type under the active bindings so return-position
  // inference (e.g. `return Option.None;`) has the expected type
  TypePtr methodReturnType;
  if (proto.hasReturnType()) {
    methodReturnType =
        analyzer_.types().typeAnnotationToType(*proto.getReturnType());
  }
  // A const method body sees the const view of its return type
  if (proto.isConstMethod())
    methodReturnType = analyzer_.types().createConstView(methodReturnType);
  ctx_.enterFunctionScope(
      methodSig, classType->getQualifiedName().memberNamed(proto.getName()),
      proto.canThrow(), methodReturnType);
  if (classType) {
    ctx_.currentScope().declareVariable("this", classType, /*isParam=*/true,
                                        /*isConst=*/proto.isConstMethod());
  }

  analyzer_.clearResolvedTypes(const_cast<BlockExprAST&>(methodFunc.getBody()));
  const auto& statements = methodFunc.getBody().getBody();
  for (size_t i = 0; i < methodFunc.getFieldInitializerCount(); ++i) {
    analyzer_.analyzeExpr(*statements.at(i));
  }

  // Step 5: Declare method parameters with substituted types
  for (size_t i = 0; i < proto.getArgs().size(); ++i) {
    const auto& [argName, argType] = proto.getArgs()[i];
    ctx_.currentScope().declareVariable(
        argName, substitutedParamTypes[i], true, false,
        proto.declarationIdentity().parameters.at(i));
  }

  // Analyze the source body after its parameters are in scope.
  for (size_t i = methodFunc.getFieldInitializerCount(); i < statements.size();
       ++i) {
    analyzer_.analyzeExpr(*statements[i]);
  }

  // Step 7: Pop scopes and restore context
  ctx_.exitScope();  // method scope
  if (needsTypeParamScope) {
    ctx_.exitScope();  // type param scope
  }
  ctx_.setCurrentClass(savedClass);
}

}  // namespace sun::semantic_analysis
