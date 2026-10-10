/** Checks analysis control flow within the semantic session. */
#include "semantic_analysis/expression_analysis/expression_properties.h"
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

void BodyAnalyzer::analyzeIfExpr(sun::ast::IfExprAST& ifExpr) {
  sema_.analyzeExpr(*ifExpr.getCond());

  // Check for type guard pattern: _is<T>(var)
  auto typeGuard = extractTypeGuard(*ifExpr.getCond());
  if (typeGuard) {
    // Apply type narrowing in the then-block
    ctx_.enterScope();
    ctx_.narrowVariable(typeGuard->first, typeGuard->second);
    sema_.analyzeExpr(*ifExpr.getThen());
    ctx_.exitScope();
  } else {
    sema_.analyzeExpr(*ifExpr.getThen());
  }

  if (ifExpr.getElse()) {
    sema_.analyzeExpr(*ifExpr.getElse());
  }

  ifExpr.setResolvedType(Types::Void());
}

void BodyAnalyzer::analyzeMatchExpr(sun::ast::MatchExprAST& matchExpr,
                                    TypePtr expectedType) {
  // Analyze the discriminant expression
  sema_.analyzeExpr(const_cast<ExprAST&>(*matchExpr.getDiscriminant()));

  // Enum discriminants get variant patterns, payload bindings, and
  // exhaustiveness checking
  TypePtr discType = unwrapRef(matchExpr.getDiscriminant()->getResolvedType());
  if (matchExpr.isPropagation()) {
    using namespace sun::ast;
    auto source = std::dynamic_pointer_cast<sun::types::EnumType>(discType);
    auto target = std::dynamic_pointer_cast<sun::types::EnumType>(
        ctx_.currentFunctionReturnType());
    const auto location = matchExpr.getLocation();
    // The first declaration is success, regardless of its name or numeric tag.
    auto validate = [&](const std::shared_ptr<sun::types::EnumType>& type,
                        const std::string& role) {
      if (!type || type->getVariants().empty())
        logAndThrowError("'try' requires " + role + " to be a nonempty enum",
                         location);
      if (type->getVariants().front().payloadTypes.size() > 1)
        logAndThrowError(
            "'try' requires the first enum variant to have zero or "
            "one success payload",
            location);
      for (size_t i = 1; i < type->getVariants().size(); ++i)
        if (type->getVariants()[i].payloadTypes.size() != 1)
          logAndThrowError(
              "'try' requires each failure variant to have one "
              "payload",
              location);
    };
    validate(source, "its operand");
    validate(target, "the enclosing function's return type");
    ctx_.currentScope().declareEnum("$try_source", source);
    ctx_.currentScope().declareEnum("$try_target", target);
    auto member = [&](const std::string& type, const std::string& variant) {
      auto name = std::make_unique<VariableReferenceAST>(type);
      name->setLocation(location);
      auto access = std::make_unique<MemberAccessAST>(std::move(name), variant);
      access->setLocation(location);
      return access;
    };
    auto construct = [&](const std::string& type, const std::string& variant,
                         std::unique_ptr<ExprAST> payload) {
      std::vector<std::unique_ptr<ExprAST>> arguments;
      arguments.push_back(std::move(payload));
      auto call = std::make_unique<CallExprAST>(member(type, variant),
                                                std::move(arguments));
      call->setLocation(location);
      return call;
    };
    auto& arms = matchExpr.getArmsMutable();
    arms.clear();
    for (size_t i = 0; i < source->getVariants().size(); ++i) {
      const auto& variant = source->getVariants()[i];
      const bool success = i == 0;
      const std::string bindingName = success ? "$try_value" : "$try_error";
      std::unique_ptr<ExprAST> body;
      if (variant.payloadTypes.empty()) {
        body = std::make_unique<BlockExprAST>();
      } else {
        body = std::make_unique<VariableReferenceAST>(bindingName);
        body->setLocation(location);
      }
      if (!success) {
        const sun::types::EnumVariant* destination = nullptr;
        const sun::types::EnumVariant* wrapper = nullptr;
        std::shared_ptr<sun::types::EnumType> wrapperType;
        if (source->equals(*target)) {
          destination = &target->getVariants()[i];
        } else {
          for (size_t j = 1; j < target->getVariants().size(); ++j) {
            const auto& candidate = target->getVariants()[j];
            if (!candidate.payloadTypes.front()->equals(
                    *variant.payloadTypes.front()))
              continue;
            if (destination)
              logAndThrowError(
                  "Ambiguous error propagation: multiple variants "
                  "accept this error",
                  location);
            destination = &candidate;
          }
          // Preserve explicit error-sum wrapping used by _Result<T, E> callers.
          if (!destination) {
            for (size_t j = 1; j < target->getVariants().size(); ++j) {
              const auto& candidate = target->getVariants()[j];
              auto nested = std::dynamic_pointer_cast<sun::types::EnumType>(
                  candidate.payloadTypes.front());
              if (!nested) continue;
              for (const auto& nestedVariant : nested->getVariants()) {
                if (nestedVariant.payloadTypes.size() != 1 ||
                    !nestedVariant.payloadTypes.front()->equals(
                        *variant.payloadTypes.front()))
                  continue;
                if (destination)
                  logAndThrowError(
                      "Ambiguous error propagation: multiple variants "
                      "accept this error",
                      location);
                destination = &candidate;
                wrapper = &nestedVariant;
                wrapperType = nested;
              }
            }
          }
        }
        if (!destination)
          logAndThrowError("Cannot propagate error variant '" + variant.name +
                               "' into the enclosing enum return type",
                           location);
        if (wrapper) {
          const auto wrapperName = "$try_wrapper_" + std::to_string(i);
          ctx_.currentScope().declareEnum(wrapperName, wrapperType);
          body = construct(wrapperName, wrapper->name, std::move(body));
        }
        body = std::make_unique<ReturnExprAST>(
            construct("$try_target", destination->name, std::move(body)));
      }
      body->setLocation(location);
      arms.emplace_back(member("$try_source", variant.name), false,
                        std::move(body));
      if (!variant.payloadTypes.empty()) {
        arms.back().hasPayloadParens = true;
        PatternBinding binding;
        binding.name = bindingName;
        binding.location = location;
        arms.back().bindings.push_back(std::move(binding));
      }
    }
  }
  if (discType && discType->isEnum()) {
    sema_.enums().analyzeEnumMatch(
        matchExpr, std::static_pointer_cast<sun::types::EnumType>(discType),
        expectedType);
    matchExpr.setResolvedType(
        matchExpr.isPropagation()
            ? (std::static_pointer_cast<sun::types::EnumType>(discType)
                       ->getVariants()
                       .front()
                       .payloadTypes.empty()
                   ? Types::Void()
                   : std::static_pointer_cast<sun::types::EnumType>(discType)
                         ->getVariants()
                         .front()
                         .payloadTypes.front())
            : preparedMatchType(matchExpr));
    checkOwnedMatchArmTypes(matchExpr);
    return;
  }

  // Analyze each arm, propagating expectedType to arm bodies
  for (auto& arm : matchExpr.getArmsMutable()) {
    if (arm.bindingType) {
      if (!arm.bindingType->isReference())
        logAndThrowError("Typed match bindings must use ref or const ref",
                         arm.pattern->getLocation());
      auto bindingType =
          sema_.typeResolver().typeAnnotationToType(*arm.bindingType);
      auto target = unwrapRef(bindingType);
      bool matches =
          discType && (discType->isTypeParameter() ||
                       (discType->isClass() && discType->equals(*target)));
      if (discType && discType->isClass() && target->isInterface())
        matches =
            static_cast<ClassType*>(discType.get())
                ->implementsInterface(
                    *static_cast<sun::types::InterfaceType*>(target.get()));
      if (!matches)
        logAndThrowError(
            "Typed match pattern does not match the concrete discriminant",
            arm.pattern->getLocation());
      auto source = matchExpr.getDiscriminant()->getResolvedType();
      if (sun::types::isMutableRef(bindingType) && source->isReference() &&
          !sun::types::isMutableRef(source))
        logAndThrowError(
            "Cannot bind a mutable reference through a const match "
            "discriminant",
            arm.pattern->getLocation());
      sema_.expressions().checkMethodReceiver(
          *matchExpr.getDiscriminant(), "typed match binding",
          !sun::types::isMutableRef(bindingType), false,
          arm.pattern->getLocation());
      arm.matchedVariantTags = {0};
      arm.pattern->setResolvedType(discType);
      auto& binding = arm.bindings.front();
      binding.resolvedType = bindingType;
      ctx_.enterScope();
      if (!binding.isWildcard)
        ctx_.currentScope().declareVariable(binding.name, bindingType, false,
                                            false, binding.declaration.id);
      sema_.analyzeExpr(*arm.body, expectedType);
      ctx_.exitScope();
      continue;
    }
    if (arm.hasPayloadParens) {
      logAndThrowError(
          "Destructuring patterns require an enum discriminant",
          arm.pattern ? arm.pattern->getLocation() : matchExpr.getLocation());
    }
    if (arm.pattern) {
      sema_.analyzeExpr(const_cast<ExprAST&>(*arm.pattern));
    }
    sema_.analyzeExpr(const_cast<ExprAST&>(*arm.body), expectedType);
  }
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
  checkOwnedMatchCoverage(matchExpr, discType);
  checkOwnedMatchArmTypes(matchExpr);
}

void BodyAnalyzer::analyzeForLoop(sun::ast::ForExprAST& forExpr) {
  // Create scope for loop variables (init may declare variables)
  ctx_.enterScope();
  if (forExpr.getInit()) {
    sema_.analyzeExpr(const_cast<ExprAST&>(*forExpr.getInit()));
  }
  if (forExpr.getCondition()) {
    sema_.analyzeExpr(const_cast<ExprAST&>(*forExpr.getCondition()));
  }
  if (forExpr.getIncrement()) {
    sema_.analyzeExpr(const_cast<ExprAST&>(*forExpr.getIncrement()));
  }
  sema_.analyzeExpr(const_cast<ExprAST&>(*forExpr.getBody()));
  ctx_.exitScope();
  forExpr.setResolvedType(Types::Float64());  // for loops return 0.0
}

void BodyAnalyzer::analyzeForInLoop(sun::ast::ForInExprAST& forInExpr) {
  // Analyze the iterable expression
  sema_.analyzeExpr(const_cast<ExprAST&>(*forInExpr.getIterable()));

  // Get the type of the iterable
  auto iterableType = forInExpr.getIterable()->getResolvedType();

  // Convert loop variable type annotation to type
  auto loopVarType =
      sema_.typeResolver().typeAnnotationToType(forInExpr.getLoopVarType());
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
  sema_.analyzeExpr(const_cast<ExprAST&>(*forInExpr.getBody()));
  ctx_.exitScope();

  forInExpr.setResolvedType(Types::Float64());  // for-in loops return 0.0
}

void BodyAnalyzer::analyzeTryCatch(sun::ast::TryCatchExprAST& tryCatchExpr) {
  // Track that we're inside a try block for error propagation checking
  ctx_.enterTryBlock();

  // Analyze the try block
  sema_.bodies().analyzeBlock(
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
          "catch clause requires a typed binding, e.g. catch (e: ref IError) "
          "{ ... }",
          tryCatchExpr.getLocation());
    }

    if (!catchClause.bindingType->isReference())
      logAndThrowError("Catch bindings must use ref or const ref",
                       tryCatchExpr.getLocation());
    TypePtr bindingType =
        sema_.typeResolver().typeAnnotationToType(*catchClause.bindingType);
    TypePtr errorType = sun::types::unwrapRef(bindingType);
    bool isCatchAll = errorType && errorType->equals(*builtinIError);
    bool valid = isCatchAll || (errorType && errorType->isClass() &&
                                static_cast<ClassType*>(errorType.get())
                                    ->implementsInterface(*builtinIError));
    if (!valid)
      logAndThrowError(
          "catch type must reference IError or a class implementing IError, "
          "got '" +
              bindingType->toDisplayString() + "'",
          tryCatchExpr.getLocation());

    // A catch-all (IError) makes any following clause unreachable.
    if (sawCatchAll) {
      logAndThrowError(
          "unreachable catch clause: a 'catch (e: ref IError)' catch-all must "
          "be the last handler",
          tryCatchExpr.getLocation());
    }
    if (isCatchAll) sawCatchAll = true;

    // Record resolution for codegen's typed matching.
    catchClause.isCatchAll = isCatchAll;
    catchClause.resolvedType = bindingType;

    ctx_.enterScope();
    ctx_.currentScope().catchBinding = catchClause.declaration.id;
    ctx_.currentScope().declareVariable(catchClause.bindingName, bindingType,
                                        false, false,
                                        catchClause.declaration.id);
    sema_.bodies().analyzeBlock(
        const_cast<sun::ast::BlockExprAST&>(*catchClause.body));
    ctx_.exitScope();
  }

  // A try-catch is a statement, not a value: code that wants a value out of
  // one returns from inside the try (see the block-kind rule on BlockKind)
  tryCatchExpr.setResolvedType(Types::Void());
}

void BodyAnalyzer::analyzeThrowExpr(sun::ast::ThrowExprAST& throwExpr) {
  // Validate that throw is used inside a function declared with "throws IError"
  if (!ctx_.isInThrowingFunction()) {
    logAndThrowError(
        "throw can only be used in functions declared with 'throws IError'",
        throwExpr.getLocation());
  }

  // Analyze the error expression being thrown
  sema_.analyzeExpr(const_cast<ExprAST&>(throwExpr.getErrorExpr()));

  TypePtr errorType = requireResolvedType(throwExpr.getErrorExpr());
  if (errorType && errorType->isReference()) {
    // Only the innermost handler's binding names the runtime's active
    // exception.
    DeclarationId binding;
    for (auto* scope = ctx_.scope(); scope; scope = scope->parent) {
      if (scope->catchBinding) {
        binding = scope->catchBinding;
        break;
      }
      if (scope->asFunction()) break;
    }
    const auto& value = throwExpr.getErrorExpr();
    if (!binding ||
        value.getType() != sun::ast::ASTNodeType::VARIABLE_REFERENCE ||
        static_cast<const sun::ast::VariableReferenceAST&>(value)
                .getTargetDeclarationId() != binding)
      logAndThrowError(
          "Cannot throw a borrowed error; only the innermost catch binding may "
          "be rethrown",
          throwExpr.getLocation());
  } else if (!errorType || !errorType->isClass() ||
             !static_cast<ClassType*>(errorType.get())
                  ->implementsInterface(*ctx_.types()->errorInterface)) {
    logAndThrowError(
        "throw expression must own a concrete type implementing IError",
        throwExpr.getLocation());
  } else {
    sema_.expressions().checkMoveSource(throwExpr.getErrorExpr(),
                                        throwExpr.getLocation());
  }

  // Throw doesn't return a value
  throwExpr.setResolvedType(Types::Void());
}

void BodyAnalyzer::analyzeUnsafeBlock(sun::ast::UnsafeBlockAST& unsafeBlock) {
  // Track that we're inside an unsafe block
  ctx_.enterUnsafeBlock();

  // Analyze the body
  sema_.bodies().analyzeBlock(unsafeBlock.getBody());

  // Exit unsafe block tracking
  ctx_.exitUnsafeBlock();

  const auto& body = unsafeBlock.getBody();
  unsafeBlock.setResolvedType(
      body.isEmpty() ? Types::Void()
                     : requireResolvedType(*body.getBody().back()));
}

void BodyAnalyzer::analyzeReturnExpr(sun::ast::ReturnExprAST& returnExpr) {
  auto* function = ctx_.currentFunctionScope();
  const bool fallibleInit = ctx_.getCurrentClass() && function &&
                            function->functionName.baseName == "init" &&
                            ctx_.currentFunctionReturnType() &&
                            ctx_.currentFunctionReturnType()->isEnum();
  auto initResult = fallibleInit
                        ? std::static_pointer_cast<sun::types::EnumType>(
                              ctx_.currentFunctionReturnType())
                        : nullptr;
  if (fallibleInit && !returnExpr.hasValue()) {
    ctx_.currentScope().declareEnum("$init_result", initResult);
    returnExpr.forEachChildSlot([&](std::unique_ptr<ExprAST>& value) {
      value = std::make_unique<sun::ast::MemberAccessAST>(
          std::make_unique<sun::ast::VariableReferenceAST>("$init_result"),
          initResult->getVariants().front().name);
      value->setLocation(returnExpr.getLocation());
    });
  }
  if (fallibleInit) {
    const ExprAST* variant = returnExpr.getValue();
    if (auto* call = dynamic_cast<const sun::ast::CallExprAST*>(variant))
      variant = call->getCallee();
    auto* member = dynamic_cast<const sun::ast::MemberAccessAST*>(variant);
    if (!member || !initResult->hasVariant(member->getMemberName()))
      logAndThrowError("A fallible init returns a result variant or return;",
                       returnExpr.getLocation());
  }
  if (returnExpr.hasValue()) {
    // Propagate the function's return type for return-position inference
    // (e.g. `return Option.None;`)
    TypePtr declaredReturn = ctx_.currentFunctionReturnType();
    returnExpr.setTargetType(declaredReturn);
    sema_.analyzeExpr(const_cast<ExprAST&>(*returnExpr.getValue()),
                      declaredReturn);
    TypePtr valueType = requireResolvedType(*returnExpr.getValue());
    if (declaredReturn && declaredReturn->isEnum() &&
        !declaredReturn->equals(*valueType))
      logAndThrowError("Return value has type '" +
                           valueType->toDisplayString() + "', expected '" +
                           declaredReturn->toDisplayString() + "'",
                       returnExpr.getLocation());
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
      sema_.expressions().checkMoveSource(*returnExpr.getValue(),
                                          returnExpr.getLocation());
    }
    returnExpr.setResolvedType(valueType);
  } else {
    TypePtr declaredReturn = ctx_.currentFunctionReturnType();
    if (declaredReturn && !declaredReturn->isVoid())
      logAndThrowError("Return requires a value of type '" +
                           declaredReturn->toDisplayString() + "'",
                       returnExpr.getLocation());
    returnExpr.setResolvedType(Types::Void());
  }
}

}  // namespace sun::semantic_analysis
