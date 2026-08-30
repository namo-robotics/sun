// analysis_control_flow.cpp — Control flow: branches, loops and matches,
// plus the two block forms that change what the code inside them may do
//
// One handler per AST node kind, called from the dispatcher in
// analysis.cpp.

#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_rules.h"
#include "support/error.h"

using sun::unwrapRef;
using sun::rules::tryCoerceIntegerLiteral;
using sun::rules::unifyTernaryTypes;

void SemanticAnalyzer::analyzeIfExpr(IfExprAST& ifExpr) {
  analyzeExpr(*ifExpr.getCond());

  // Check for type guard pattern: _is<T>(var)
  auto typeGuard = extractTypeGuard(*ifExpr.getCond());
  if (typeGuard) {
    // Apply type narrowing in the then-block
    ctx_.enterScope();
    ctx_.narrowVariable(typeGuard->first, typeGuard->second);
    analyzeExpr(*ifExpr.getThen());
    ctx_.exitScope();
  } else {
    analyzeExpr(*ifExpr.getThen());
  }

  if (ifExpr.getElse()) {
    analyzeExpr(*ifExpr.getElse());
  }

  // If expression type: use types_.inferType(it handles with/without else)
  ifExpr.setResolvedType(types_.inferType(ifExpr));
}

void SemanticAnalyzer::analyzeMatchExpr(MatchExprAST& matchExpr,
                                        sun::TypePtr expectedType) {
  // Analyze the discriminant expression
  analyzeExpr(const_cast<ExprAST&>(*matchExpr.getDiscriminant()));

  // Enum discriminants get variant patterns, payload bindings, and
  // exhaustiveness checking
  sun::TypePtr discType =
      unwrapRef(matchExpr.getDiscriminant()->getResolvedType());
  if (discType && discType->isEnum()) {
    analyzeEnumMatch(matchExpr,
                     std::static_pointer_cast<sun::EnumType>(discType),
                     expectedType);
    matchExpr.setResolvedType(types_.inferType(matchExpr));
    return;
  }

  // Analyze each arm, propagating expectedType to arm bodies
  for (const auto& arm : matchExpr.getArms()) {
    if (arm.hasPayloadParens) {
      logAndThrowError(
          "Destructuring patterns require an enum discriminant",
          arm.pattern ? arm.pattern->getLocation() : matchExpr.getLocation());
    }
    if (arm.pattern) {
      analyzeExpr(const_cast<ExprAST&>(*arm.pattern));
    }
    analyzeExpr(const_cast<ExprAST&>(*arm.body), expectedType);
  }
  // If we have an expected type and all arms resolved to it, use it
  if (expectedType) {
    bool allArmsMatch = true;
    for (const auto& arm : matchExpr.getArms()) {
      if (arm.body->getResolvedType() != expectedType) {
        allArmsMatch = false;
        break;
      }
    }
    if (allArmsMatch) {
      matchExpr.setResolvedType(expectedType);
      return;
    }
  }
  matchExpr.setResolvedType(types_.inferType(matchExpr));
}

void SemanticAnalyzer::analyzeTernaryExpr(TernaryExprAST& ternary,
                                          sun::TypePtr expectedType) {
  // Condition is not required to be bool (matches if/while laxness);
  // codegen coerces numeric conditions to i1.
  analyzeExpr(*ternary.getCond());
  analyzeExpr(*ternary.getThen(), expectedType);
  analyzeExpr(*ternary.getElse(), expectedType);

  sun::TypePtr thenType = sun::unwrapRef(ternary.getThen()->getResolvedType());
  sun::TypePtr elseType = sun::unwrapRef(ternary.getElse()->getResolvedType());

  // Integer literals adopt the other branch's type: c ? x : 0
  if (thenType && elseType && !thenType->equals(*elseType)) {
    if (tryCoerceIntegerLiteral(ternary.getThen(), elseType, false)) {
      thenType = elseType;
    } else if (tryCoerceIntegerLiteral(ternary.getElse(), thenType, false)) {
      elseType = thenType;
    }
  }

  ternary.setResolvedType(
      unifyTernaryTypes(thenType, elseType, ternary.getLocation()));
}

void SemanticAnalyzer::analyzeForLoop(ForExprAST& forExpr) {
  // Create scope for loop variables (init may declare variables)
  ctx_.enterScope();
  if (forExpr.getInit()) {
    analyzeExpr(const_cast<ExprAST&>(*forExpr.getInit()));
  }
  if (forExpr.getCondition()) {
    analyzeExpr(const_cast<ExprAST&>(*forExpr.getCondition()));
  }
  if (forExpr.getIncrement()) {
    analyzeExpr(const_cast<ExprAST&>(*forExpr.getIncrement()));
  }
  analyzeExpr(const_cast<ExprAST&>(*forExpr.getBody()));
  ctx_.exitScope();
  forExpr.setResolvedType(sun::Types::Float64());  // for loops return 0.0
}

void SemanticAnalyzer::analyzeForInLoop(ForInExprAST& forInExpr) {
  // Analyze the iterable expression
  analyzeExpr(const_cast<ExprAST&>(*forInExpr.getIterable()));

  // Get the type of the iterable
  auto iterableType = forInExpr.getIterable()->getResolvedType();

  // Convert loop variable type annotation to type
  auto loopVarType = types_.typeAnnotationToType(forInExpr.getLoopVarType());
  forInExpr.setResolvedLoopVarType(loopVarType);

  // Verify the iterable type implements IIterator<T> or IIterable<T>
  auto classType = std::dynamic_pointer_cast<sun::ClassType>(iterableType);
  if (!classType) {
    logAndThrowError(
        "for-in loop requires a class type that implements IIterator<T> "
        "or IIterable<T>",
        forInExpr.getLocation());
  }
  bool implementsIterator = false;
  bool implementsIterable = false;
  // Interface names are mangled and may be module-qualified
  // (sun_IIterator_i32_Range); match on the base name
  for (const auto& ifaceName : classType->getImplementedInterfaces()) {
    if (ifaceName.find("IIterator_") != std::string::npos ||
        ifaceName.find("IIterator<") != std::string::npos ||
        ifaceName == "IIterator") {
      implementsIterator = true;
      break;
    }
    if (ifaceName.find("IIterable_") != std::string::npos ||
        ifaceName.find("IIterable<") != std::string::npos ||
        ifaceName == "IIterable") {
      implementsIterable = true;
      break;
    }
  }
  if (!implementsIterator && !implementsIterable) {
    logAndThrowError(
        "for-in loop requires type that implements IIterator<T> or "
        "IIterable<T>, but '" +
            classType->getDisplayName() + "' does not implement either",
        forInExpr.getLocation());
  }

  // Resolve the iterator class: the iterable itself, or what iter()
  // returns. Codegen relies on these shapes, so they are all errors here.
  std::shared_ptr<sun::ClassType> iteratorType = classType;
  if (!implementsIterator) {
    const auto* iterMethod = classType->getMethod("iter");
    iteratorType = iterMethod ? std::dynamic_pointer_cast<sun::ClassType>(
                                    sun::unwrapRef(iterMethod->returnType))
                              : nullptr;
    if (!iteratorType) {
      logAndThrowError("for-in loop: '" + classType->getDisplayName() +
                           "' must define iter() returning an iterator "
                           "class",
                       forInExpr.getLocation());
    }
  }
  const sun::ClassMethod* nextMethod = iteratorType->getMethod("next");
  if (!nextMethod || !nextMethod->returnType) {
    logAndThrowError("for-in loop: iterator '" +
                         iteratorType->getDisplayName() +
                         "' must define next(container: ref " +
                         classType->getDisplayName() + ") Option<T>",
                     forInExpr.getLocation());
  }

  // next() takes exactly the iterable by ref: codegen passes the
  // iterable's address, so any other parameter type would reinterpret it
  bool containerOk = nextMethod->paramTypes.size() == 1 &&
                     nextMethod->paramTypes[0] &&
                     nextMethod->paramTypes[0]->isReference();
  if (containerOk) {
    sun::TypePtr paramType = sun::unwrapRef(nextMethod->paramTypes[0]);
    containerOk = paramType && (paramType->isTypeParameter() ||
                                paramType->equals(*classType));
  }
  if (!containerOk) {
    logAndThrowError("for-in loop: iterator '" +
                         iteratorType->getDisplayName() +
                         "' must take the iterable by reference: "
                         "next(container: ref " +
                         classType->getDisplayName() + ")",
                     forInExpr.getLocation());
  }

  // The element type is the payload of next()'s Option<T>; the loop
  // variable annotation must agree with it
  sun::TypePtr elementType;
  if (auto* opt = dynamic_cast<sun::EnumType*>(
          sun::unwrapRef(nextMethod->returnType).get())) {
    const sun::EnumVariant* some = opt->getVariant("Some");
    if (some && some->payloadTypes.size() == 1 && opt->hasVariant("None")) {
      elementType = some->payloadTypes[0];
    }
  }
  if (!elementType) {
    logAndThrowError("for-in loop: iterator '" +
                         iteratorType->getDisplayName() +
                         "' must return Option<T> from next(), got '" +
                         nextMethod->returnType->toDisplayString() + "'",
                     forInExpr.getLocation());
  }
  // An iterator that yields Option<ref X> borrows: `for (var x: X in c)`
  // binds x to the element in place rather than copying it out, which is
  // what lets a container be iterated without duplicating elements it
  // still owns. Writing `ref X` in the annotation says the same thing.
  if (elementType->isReference() && loopVarType &&
      !loopVarType->isReference() &&
      sun::unwrapRef(elementType)->equals(*loopVarType)) {
    loopVarType = elementType;
    forInExpr.setResolvedLoopVarType(loopVarType);
  }
  if (loopVarType && !elementType->isTypeParameter() &&
      !loopVarType->isTypeParameter() && !elementType->equals(*loopVarType)) {
    logAndThrowError("for-in loop variable '" + forInExpr.getLoopVar() +
                         "' has type '" + loopVarType->toDisplayString() +
                         "' but the iterator yields '" +
                         elementType->toDisplayString() + "'",
                     forInExpr.getLocation());
  }

  // Create scope for loop body with loop variable
  ctx_.enterScope();
  ctx_.declareVariable(forInExpr.getLoopVar(), loopVarType, /*isParam=*/false,
                       forInExpr.isConst());
  analyzeExpr(const_cast<ExprAST&>(*forInExpr.getBody()));
  ctx_.exitScope();

  forInExpr.setResolvedType(sun::Types::Float64());  // for-in loops return 0.0
}

void SemanticAnalyzer::analyzeTryCatch(TryCatchExprAST& tryCatchExpr) {
  // Track that we're inside a try block for error propagation checking
  ctx_.enterTryBlock();

  // Analyze the try block
  analyzeBlock(const_cast<BlockExprAST&>(tryCatchExpr.getTryBlock()));

  // Exit try block tracking
  ctx_.exitTryBlock();

  // Analyze each catch clause, tested in source order.
  auto builtinIError = ctx_.types()->getInterface("IError");
  bool sawCatchAll = false;
  auto& clauses = tryCatchExpr.getCatchClausesMutable();
  for (auto& catchClause : clauses) {
    if (catchClause.bindingName.empty() ||
        !catchClause.bindingType.has_value()) {
      logAndThrowError(
          "catch clause requires a typed binding, e.g. catch (e: IError) "
          "{ ... }",
          tryCatchExpr.getLocation());
    }

    sun::TypePtr bindingType =
        types_.typeAnnotationToType(*catchClause.bindingType);

    // The catch type must be IError or a class implementing IError.
    bool isCatchAll = false;
    bool valid = false;
    if (bindingType && bindingType->isInterface()) {
      if (bindingType.get() == builtinIError.get()) {
        valid = true;
        isCatchAll = true;  // catch (e: IError) matches any error
      }
    } else if (bindingType && bindingType->isClass()) {
      valid = static_cast<sun::ClassType*>(bindingType.get())
                  ->implementsInterface("IError");
    }
    if (!valid) {
      logAndThrowError(
          "catch type must be 'IError' or a class implementing IError, "
          "got '" +
              (bindingType ? bindingType->toDisplayString()
                           : std::string("?")) +
              "'",
          tryCatchExpr.getLocation());
    }

    // A catch-all (IError) makes any following clause unreachable.
    if (sawCatchAll) {
      logAndThrowError(
          "unreachable catch clause: a 'catch (e: IError)' catch-all must "
          "be the last handler",
          tryCatchExpr.getLocation());
    }
    if (isCatchAll) sawCatchAll = true;

    // Record resolution for codegen's typed matching.
    catchClause.isCatchAll = isCatchAll;
    catchClause.resolvedMangledName =
        isCatchAll
            ? std::string()
            : static_cast<sun::ClassType*>(bindingType.get())->getMangledName();

    ctx_.enterScope();
    ctx_.declareVariable(catchClause.bindingName, bindingType);
    analyzeBlock(const_cast<BlockExprAST&>(*catchClause.body));
    ctx_.exitScope();
  }

  // A try-catch is a statement, not a value: code that wants a value out of
  // one returns from inside the try (see the block-kind rule on BlockKind)
  tryCatchExpr.setResolvedType(sun::Types::Void());
}

void SemanticAnalyzer::analyzeThrowExpr(ThrowExprAST& throwExpr) {
  // Validate that throw is used inside a function declared with "throws IError"
  if (!ctx_.isInThrowingFunction()) {
    logAndThrowError(
        "throw can only be used in functions declared with 'throws IError'",
        throwExpr.getLocation());
  }

  // Analyze the error expression being thrown
  analyzeExpr(const_cast<ExprAST&>(throwExpr.getErrorExpr()));

  // Validate that the thrown expression implements IError
  sun::TypePtr errorType = types_.inferType(throwExpr.getErrorExpr());
  if (errorType) {
    bool implementsIError = false;

    // Get the builtin IError interface for comparison
    auto builtinIError = ctx_.types()->getInterface("IError");

    // Check if it's the IError interface itself (e.g., re-throwing caught
    // error)
    if (errorType->isInterface()) {
      // IError itself is throwable
      if (errorType.get() == builtinIError.get()) {
        implementsIError = true;
      }
    }
    // Check if it's a class that implements IError
    else if (errorType->isClass()) {
      auto* classType = static_cast<sun::ClassType*>(errorType.get());
      implementsIError = classType->implementsInterface("IError");
    }
    // Check if it's a reference to a class that implements IError
    else if (errorType->isReference()) {
      auto* refType = static_cast<sun::ReferenceType*>(errorType.get());
      sun::TypePtr innerType = refType->getReferencedType();
      if (innerType && innerType->isClass()) {
        auto* classType = static_cast<sun::ClassType*>(innerType.get());
        implementsIError = classType->implementsInterface("IError");
      }
      // Also allow reference to IError interface
      else if (innerType && innerType->isInterface()) {
        if (innerType.get() == builtinIError.get()) {
          implementsIError = true;
        }
      }
    }

    if (!implementsIError) {
      logAndThrowError(
          "throw expression must be a type implementing IError, got '" +
              errorType->toDisplayString() + "'",
          throwExpr.getLocation());
    }
  }

  // Throw doesn't return a value
  throwExpr.setResolvedType(sun::Types::Void());
}

void SemanticAnalyzer::analyzeUnsafeBlock(UnsafeBlockAST& unsafeBlock) {
  // Track that we're inside an unsafe block
  ctx_.enterUnsafeBlock();

  // Analyze the body
  analyzeBlock(unsafeBlock.getBody());

  // Exit unsafe block tracking
  ctx_.exitUnsafeBlock();

  // Infer the result type (inferType handles unsafe context internally)
  sun::TypePtr resultType = types_.inferType(unsafeBlock);
  unsafeBlock.setResolvedType(resultType ? resultType : sun::Types::Void());
}

void SemanticAnalyzer::analyzeReturnExpr(ReturnExprAST& returnExpr) {
  if (returnExpr.hasValue()) {
    // Propagate the function's return type for return-position inference
    // (e.g. `return Option.None;`)
    sun::TypePtr declaredReturn = ctx_.currentFunctionReturnType();
    analyzeExpr(const_cast<ExprAST&>(*returnExpr.getValue()), declaredReturn);
    sun::TypePtr valueType = types_.inferType(*returnExpr.getValue());
    // Returning by value out of a borrow would hand the caller a second
    // value backed by the borrowed storage.
    if (valueType && valueType->isReference() && declaredReturn &&
        !declaredReturn->isReference() &&
        !sun::typeCopiesByRead(declaredReturn)) {
      logAndThrowError(
          "Cannot return a borrowed '" + declaredReturn->toDisplayString() +
              "' by value: reading it out of the borrow would copy it. "
              "Return 'ref " +
              declaredReturn->toDisplayString() +
              "' to keep borrowing, copy it explicitly with clone(), or "
              "move the value out first (take()/pop()/remove() on a "
              "container).",
          returnExpr.getLocation());
    }
    if (!declaredReturn || !declaredReturn->isReference()) {
      checkMoveSource(*returnExpr.getValue(), returnExpr.getLocation());
    }
    returnExpr.setResolvedType(valueType);
  } else {
    returnExpr.setResolvedType(sun::Types::Void());
  }
}
