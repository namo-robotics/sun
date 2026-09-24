// analysis_control_flow.cpp — Control flow: branches, loops and matches,
// plus the two block forms that change what the code inside them may do
//
// One handler per AST node kind, called from the dispatcher in
// analysis.cpp.

#include "ast/control_flow.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_analysis/type_rules.h"
#include "support/error.h"

using sun::types::ClassType;
using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::ExprAST;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

using sun::semantic_analysis::type_analysis::tryCoerceIntegerLiteral;
using sun::semantic_analysis::type_analysis::unifyTernaryTypes;
using sun::types::unwrapRef;

void SemanticAnalyzer::analyzeIfExpr(sun::ast::IfExprAST& ifExpr) {
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

  ifExpr.setResolvedType(Types::Void());
}

void SemanticAnalyzer::analyzeMatchExpr(sun::ast::MatchExprAST& matchExpr,
                                        TypePtr expectedType) {
  // Analyze the discriminant expression
  analyzeExpr(const_cast<ExprAST&>(*matchExpr.getDiscriminant()));

  auto checkOwnedArmTypes = [&] {
    auto resultType = matchExpr.getResolvedType();
    if (!sun::types::typeMovesOnRead(resultType)) return;
    std::set<int64_t> coveredTags;
    for (const auto& arm : matchExpr.getArms()) {
      if (!arm.isWildcard && arm.pattern && arm.pattern->getResolvedType() &&
          arm.pattern->getResolvedType()->isEnum() &&
          !coveredTags.insert(arm.resolvedVariantTag).second)
        continue;
      if (!sun::ast::exprDiverges(*arm.body)) {
        auto armType = unwrapRef(arm.body->getResolvedType());
        if (!armType || !armType->equals(*resultType)) {
          logAndThrowError(
              "Every reachable arm must produce the same owned type",
              arm.body->getLocation());
        }
      }
      if (arm.isWildcard) break;
    }
  };

  // Enum discriminants get variant patterns, payload bindings, and
  // exhaustiveness checking
  TypePtr discType = unwrapRef(matchExpr.getDiscriminant()->getResolvedType());
  if (discType && discType->isEnum()) {
    enums_.analyzeEnumMatch(
        matchExpr, std::static_pointer_cast<sun::types::EnumType>(discType),
        expectedType);
    matchExpr.setResolvedType(preparedMatchType(matchExpr));
    checkOwnedArmTypes();
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
  // Owned results need a value on every path; there is no empty resource
  // that code generation can safely invent for an unmatched input.
  auto checkOwnedCoverage = [&] {
    if (!sun::types::typeMovesOnRead(matchExpr.getResolvedType())) return;
    bool hasTrue = false;
    bool hasFalse = false;
    for (const auto& arm : matchExpr.getArms()) {
      if (arm.isWildcard) return;
      if (arm.pattern &&
          arm.pattern->getType() == sun::ast::ASTNodeType::BOOL_LITERAL) {
        if (static_cast<const sun::ast::BoolLiteralAST&>(*arm.pattern)
                .getValue()) {
          hasTrue = true;
        } else {
          hasFalse = true;
        }
      }
    }
    if (discType && discType->isBool() && hasTrue && hasFalse) return;
    logAndThrowError(
        "Match producing an owned value must cover every input; add a '_' arm",
        matchExpr.getLocation());
  };
  // If we have an expected type and all arms resolved to it, use it
  bool allArmsMatch = expectedType != nullptr;
  if (expectedType) {
    for (const auto& arm : matchExpr.getArms()) {
      if (arm.body->getResolvedType() != expectedType) {
        allArmsMatch = false;
        break;
      }
    }
  }
  matchExpr.setResolvedType(allArmsMatch ? expectedType
                                         : preparedMatchType(matchExpr));
  checkOwnedCoverage();
  checkOwnedArmTypes();
}

void SemanticAnalyzer::analyzeTernaryExpr(sun::ast::TernaryExprAST& ternary,
                                          TypePtr expectedType) {
  // Condition is not required to be bool (matches if/while laxness);
  // codegen coerces numeric conditions to i1.
  analyzeExpr(*ternary.getCond());
  analyzeExpr(*ternary.getThen(), expectedType);
  analyzeExpr(*ternary.getElse(), expectedType);

  TypePtr thenType =
      sun::types::unwrapRef(ternary.getThen()->getResolvedType());
  TypePtr elseType =
      sun::types::unwrapRef(ternary.getElse()->getResolvedType());

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

void SemanticAnalyzer::analyzeForLoop(sun::ast::ForExprAST& forExpr) {
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
  forExpr.setResolvedType(Types::Float64());  // for loops return 0.0
}

void SemanticAnalyzer::analyzeForInLoop(sun::ast::ForInExprAST& forInExpr) {
  // Analyze the iterable expression
  analyzeExpr(const_cast<ExprAST&>(*forInExpr.getIterable()));

  // Get the type of the iterable
  auto iterableType = forInExpr.getIterable()->getResolvedType();

  // Convert loop variable type annotation to type
  auto loopVarType = resolver_.typeAnnotationToType(forInExpr.getLoopVarType());
  forInExpr.setResolvedLoopVarType(loopVarType);

  // Verify the iterable type implements IIterator<T> or IIterable<T>
  auto classType = std::dynamic_pointer_cast<ClassType>(iterableType);
  if (!classType) {
    logAndThrowError(
        "for-in loop requires a class type that implements IIterator<T> "
        "or IIterable<T>",
        forInExpr.getLocation());
  }
  bool implementsIterator = false;
  bool implementsIterable = false;
  // The iteration protocols are recognized by their declarations' source names.
  for (auto interfaceId : classType->getImplementedInterfaces()) {
    const auto& instance = ctx_.results().declarations.get(interfaceId);
    const auto& source =
        instance.specialization
            ? ctx_.results().declarations.get(instance.specialization->source)
            : instance;
    if (source.name == "IIterator") implementsIterator = true;
    if (source.name == "IIterable") implementsIterable = true;
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
  std::shared_ptr<ClassType> iteratorType = classType;
  if (!implementsIterator) {
    const auto* iterMethod = classType->getMethod("iter");
    if (iterMethod)
      forInExpr.forInAnalysis().iteratorFactory = iterMethod->declarationId;
    iteratorType = iterMethod
                       ? std::dynamic_pointer_cast<ClassType>(
                             sun::types::unwrapRef(iterMethod->returnType))
                       : nullptr;
    if (!iteratorType) {
      logAndThrowError("for-in loop: '" + classType->getDisplayName() +
                           "' must define iter() returning an iterator "
                           "class",
                       forInExpr.getLocation());
    }
  }
  const sun::types::ClassMethod* nextMethod = iteratorType->getMethod("next");
  if (!nextMethod || !nextMethod->returnType) {
    logAndThrowError("for-in loop: iterator '" +
                         iteratorType->getDisplayName() +
                         "' must define next(container: ref " +
                         classType->getDisplayName() + ") Option<T>",
                     forInExpr.getLocation());
  }

  forInExpr.forInAnalysis().iteratorNext = nextMethod->declarationId;
  forInExpr.forInAnalysis().iteratorResultType = nextMethod->returnType;

  // next() takes exactly the iterable by ref: codegen passes the
  // iterable's address, so any other parameter type would reinterpret it
  bool containerOk = nextMethod->paramTypes.size() == 1 &&
                     nextMethod->paramTypes[0] &&
                     nextMethod->paramTypes[0]->isReference();
  if (containerOk) {
    TypePtr paramType = sun::types::unwrapRef(nextMethod->paramTypes[0]);
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
  TypePtr elementType;
  if (auto* opt = dynamic_cast<sun::types::EnumType*>(
          sun::types::unwrapRef(nextMethod->returnType).get())) {
    const sun::types::EnumVariant* some = opt->getVariant("Some");
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
      sun::types::unwrapRef(elementType)->equals(*loopVarType)) {
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
  ctx_.currentScope().declareVariable(forInExpr.getLoopVar(), loopVarType,
                                      /*isParam=*/false, forInExpr.isConst(),
                                      forInExpr.getDeclarationId());
  analyzeExpr(const_cast<ExprAST&>(*forInExpr.getBody()));
  ctx_.exitScope();

  forInExpr.setResolvedType(Types::Float64());  // for-in loops return 0.0
}

void SemanticAnalyzer::analyzeTryCatch(
    sun::ast::TryCatchExprAST& tryCatchExpr) {
  // Track that we're inside a try block for error propagation checking
  ctx_.enterTryBlock();

  // Analyze the try block
  bodies_.analyzeBlock(
      const_cast<sun::ast::BlockExprAST&>(tryCatchExpr.getTryBlock()));

  // Exit try block tracking
  ctx_.exitTryBlock();

  // Analyze each catch clause, tested in source order.
  auto builtinIError = ctx_.types()->errorInterface;
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

    TypePtr bindingType =
        resolver_.typeAnnotationToType(*catchClause.bindingType);

    // The catch type must be IError or a class implementing IError.
    bool isCatchAll = false;
    bool valid = false;
    if (bindingType && bindingType->isInterface()) {
      if (bindingType.get() == builtinIError.get()) {
        valid = true;
        isCatchAll = true;  // catch (e: IError) matches any error
      }
    } else if (bindingType && bindingType->isClass()) {
      valid = static_cast<ClassType*>(bindingType.get())
                  ->implementsInterface(*builtinIError);
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
    catchClause.resolvedType = bindingType;

    ctx_.enterScope();
    ctx_.currentScope().declareVariable(catchClause.bindingName, bindingType,
                                        false, false,
                                        catchClause.declaration.id);
    bodies_.analyzeBlock(
        const_cast<sun::ast::BlockExprAST&>(*catchClause.body));
    ctx_.exitScope();
  }

  // A try-catch is a statement, not a value: code that wants a value out of
  // one returns from inside the try (see the block-kind rule on BlockKind)
  tryCatchExpr.setResolvedType(Types::Void());
}

void SemanticAnalyzer::analyzeThrowExpr(sun::ast::ThrowExprAST& throwExpr) {
  // Validate that throw is used inside a function declared with "throws IError"
  if (!ctx_.isInThrowingFunction()) {
    logAndThrowError(
        "throw can only be used in functions declared with 'throws IError'",
        throwExpr.getLocation());
  }

  // Analyze the error expression being thrown
  analyzeExpr(const_cast<ExprAST&>(throwExpr.getErrorExpr()));

  // Validate that the thrown expression implements IError
  TypePtr errorType = requireResolvedType(throwExpr.getErrorExpr());
  if (errorType) {
    bool implementsIError = false;

    // Get the builtin IError interface for comparison
    auto builtinIError = ctx_.types()->errorInterface;

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
      auto* classType = static_cast<ClassType*>(errorType.get());
      implementsIError = classType->implementsInterface(*builtinIError);
    }
    // Check if it's a reference to a class that implements IError
    else if (errorType->isReference()) {
      auto* refType = static_cast<sun::types::ReferenceType*>(errorType.get());
      TypePtr innerType = refType->getReferencedType();
      if (innerType && innerType->isClass()) {
        auto* classType = static_cast<ClassType*>(innerType.get());
        implementsIError = classType->implementsInterface(*builtinIError);
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
  throwExpr.setResolvedType(Types::Void());
}

void SemanticAnalyzer::analyzeUnsafeBlock(
    sun::ast::UnsafeBlockAST& unsafeBlock) {
  // Track that we're inside an unsafe block
  ctx_.enterUnsafeBlock();

  // Analyze the body
  bodies_.analyzeBlock(unsafeBlock.getBody());

  // Exit unsafe block tracking
  ctx_.exitUnsafeBlock();

  const auto& body = unsafeBlock.getBody();
  unsafeBlock.setResolvedType(
      body.isEmpty() ? Types::Void()
                     : requireResolvedType(*body.getBody().back()));
}

void SemanticAnalyzer::analyzeReturnExpr(sun::ast::ReturnExprAST& returnExpr) {
  if (returnExpr.hasValue()) {
    // Propagate the function's return type for return-position inference
    // (e.g. `return Option.None;`)
    TypePtr declaredReturn = ctx_.currentFunctionReturnType();
    returnExpr.setTargetType(declaredReturn);
    analyzeExpr(const_cast<ExprAST&>(*returnExpr.getValue()), declaredReturn);
    TypePtr valueType = requireResolvedType(*returnExpr.getValue());
    // Returning by value out of a borrow would hand the caller a second
    // value backed by the borrowed storage.
    if (valueType && valueType->isReference() && declaredReturn &&
        !declaredReturn->isReference() &&
        !sun::types::typeCopiesByRead(declaredReturn)) {
      logAndThrowError(
          "Cannot return a borrowed '" + declaredReturn->toDisplayString() +
              "' by value: reading it out of the borrow would copy it. "
              "Return 'ref " +
              declaredReturn->toDisplayString() +
              "' to keep borrowing, copy it explicitly with clone(), or "
              "move the value out first (pop()/remove()/swap_remove() on a "
              "container).",
          returnExpr.getLocation());
    }
    if (!declaredReturn || !declaredReturn->isReference()) {
      checkMoveSource(*returnExpr.getValue(), returnExpr.getLocation());
    }
    returnExpr.setResolvedType(valueType);
  } else {
    returnExpr.setResolvedType(Types::Void());
  }
}

}  // namespace sun::semantic_analysis
