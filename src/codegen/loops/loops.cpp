// loops.cpp - Loop codegen (for loops, while loops, break, continue)

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/loops/loop_generator.h"

using namespace llvm;

Value* LoopGenerator::codegen(const ForExprAST& expr) {
  Function* func = ctx.builder->GetInsertBlock()->getParent();

  // Enter a new scope for loop variables
  scopes().push(expr.getLocation());

  // Emit the initialization code (can be var declaration or assignment)
  if (expr.getInit()) {
    codegen(*expr.getInit());
  }

  // Create basic blocks for the loop structure
  BasicBlock* condBB = BasicBlock::Create(ctx.getContext(), "loopcond", func);
  BasicBlock* bodyBB = BasicBlock::Create(ctx.getContext(), "loopbody", func);
  BasicBlock* stepBB = BasicBlock::Create(ctx.getContext(), "loopstep", func);
  BasicBlock* afterBB = BasicBlock::Create(ctx.getContext(), "afterloop", func);

  // Branch to the condition check first
  ctx.builder->CreateBr(condBB);

  // Emit condition check block
  ctx.builder->SetInsertPoint(condBB);

  // Compute the condition expression
  Value* loopCond;
  if (expr.getCondition()) {
    Value* condVal = codegen(*expr.getCondition());
    if (!condVal) return nullptr;

    // If condVal is already a boolean (i1), use it directly
    if (condVal->getType()->isIntegerTy(1)) {
      loopCond = condVal;
    } else {
      // Non-boolean: treat as truthy (non-zero)
      if (condVal->getType()->isIntegerTy()) {
        loopCond = ctx.builder->CreateICmpNE(
            condVal, ConstantInt::get(condVal->getType(), 0), "loopcond");
      } else {
        loopCond = ctx.builder->CreateFCmpONE(
            condVal, ConstantFP::get(condVal->getType(), 0.0), "loopcond");
      }
    }
  } else {
    // No condition means infinite loop (always true)
    loopCond = ConstantInt::getTrue(ctx.getContext());
  }

  // Branch: if condition true go to body, else exit
  ctx.builder->CreateCondBr(loopCond, bodyBB, afterBB);

  // Emit loop body
  ctx.builder->SetInsertPoint(bodyBB);

  // Per-iteration scope: owners declared in the body are dropped at the
  // back-edge (and by break/continue), not once after the loop
  scopes().push(expr.getBody()->getLocation());

  // Push loop context for break/continue (continue goes to step, break goes to
  // after)
  loopStack.push_back({stepBB, afterBB, scopes().size() - 1});

  // Emit the body of the loop.
  codegen(*expr.getBody());

  // Pop loop context
  loopStack.pop_back();

  // Emits this iteration's drops unless the block already terminated
  scopes().pop();

  // Only emit branch to step if current block has no terminator (break/continue
  // didn't execute)
  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    ctx.builder->CreateBr(stepBB);
  }

  // Emit step block
  ctx.builder->SetInsertPoint(stepBB);

  // Emit the increment expression if present
  if (expr.getIncrement()) {
    codegen(*expr.getIncrement());
  }

  // Jump back to condition check
  ctx.builder->CreateBr(condBB);

  // Continue inserting after the loop
  ctx.builder->SetInsertPoint(afterBB);

  // Pop the loop scope
  scopes().pop();

  // for expr always returns 0.0.
  return Constant::getNullValue(Type::getDoubleTy(ctx.getContext()));
}

Value* LoopGenerator::codegen(const WhileExprAST& expr) {
  Function* func = ctx.builder->GetInsertBlock()->getParent();

  // Create basic blocks for the loop structure
  BasicBlock* condBB = BasicBlock::Create(ctx.getContext(), "whilecond", func);
  BasicBlock* bodyBB = BasicBlock::Create(ctx.getContext(), "whilebody", func);
  BasicBlock* afterBB =
      BasicBlock::Create(ctx.getContext(), "afterwhile", func);

  // Branch to the condition check first
  ctx.builder->CreateBr(condBB);

  // Emit condition check block
  ctx.builder->SetInsertPoint(condBB);

  // Evaluate the condition
  Value* condVal = codegen(*expr.getCondition());
  if (!condVal) return nullptr;

  // Convert condition to a bool
  Value* loopCond;
  if (condVal->getType()->isIntegerTy(1)) {
    // Already a boolean
    loopCond = condVal;
  } else if (condVal->getType()->isIntegerTy()) {
    loopCond = ctx.builder->CreateICmpNE(
        condVal, ConstantInt::get(condVal->getType(), 0), "whilecond");
  } else {
    loopCond = ctx.builder->CreateFCmpONE(
        condVal, ConstantFP::get(ctx.getContext(), APFloat(0.0)), "whilecond");
  }

  // Branch: if condition true go to body, else exit
  ctx.builder->CreateCondBr(loopCond, bodyBB, afterBB);

  // Emit loop body
  ctx.builder->SetInsertPoint(bodyBB);

  // Per-iteration scope: owners declared in the body are dropped at the
  // back-edge (and by break/continue), not leaked into the enclosing scope
  scopes().push(expr.getBody()->getLocation());

  // Push loop context for break/continue (continue goes to cond, break goes to
  // after)
  loopStack.push_back({condBB, afterBB, scopes().size() - 1});

  // Emit the body of the loop.
  codegen(*expr.getBody());

  // Pop loop context
  loopStack.pop_back();

  // Emits this iteration's drops unless the block already terminated
  scopes().pop();

  // Only emit branch to condition if current block has no terminator
  // (break/continue didn't execute)
  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    ctx.builder->CreateBr(condBB);
  }

  // Continue inserting after the loop
  ctx.builder->SetInsertPoint(afterBB);

  // while expr always returns 0.0.
  return Constant::getNullValue(Type::getDoubleTy(ctx.getContext()));
}

Value* LoopGenerator::codegen(const BreakAST& expr) {
  if (loopStack.empty()) {
    logAndThrowError("'break' statement not within a loop");
    return nullptr;
  }

  // Drop owners in all scopes being jumped out of (down to the loop body)
  scopes().emitCleanupToDepth(loopStack.back().cleanupDepth);

  // Jump to the break target (the block after the loop)
  ctx.builder->CreateBr(loopStack.back().breakBlock);

  // Return a dummy value (the branch is the important part)
  return Constant::getNullValue(Type::getDoubleTy(ctx.getContext()));
}

Value* LoopGenerator::codegen(const ContinueAST& expr) {
  if (loopStack.empty()) {
    logAndThrowError("'continue' statement not within a loop");
    return nullptr;
  }

  // Drop owners in all scopes being jumped out of (down to the loop body)
  scopes().emitCleanupToDepth(loopStack.back().cleanupDepth);

  // Jump to the continue target (condition for while, step for for)
  ctx.builder->CreateBr(loopStack.back().continueBlock);

  // Return a dummy value (the branch is the important part)
  return Constant::getNullValue(Type::getDoubleTy(ctx.getContext()));
}

Value* LoopGenerator::codegen(const ForInExprAST& expr) {
  Function* func = ctx.builder->GetInsertBlock()->getParent();

  // Enter a new scope for loop variables
  scopes().push(expr.getLocation());

  // Evaluate the iterable expression to get the container/iterator. The
  // iterator protocol is next(ref Container) -> Option<T>: loop until None.
  Value* iterableObj = codegen(*expr.getIterable());
  if (!iterableObj) return nullptr;

  auto iterableType = expr.getIterable()->getResolvedType();
  if (!iterableType) {
    logAndThrowError("Could not determine type of iterable in for-in loop");
    return nullptr;
  }

  // A struct value (e.g. a method call returning by value) needs an alloca so
  // it can be passed by ref
  if (iterableObj->getType()->isStructTy()) {
    llvm::StructType* structType = cast<StructType>(iterableObj->getType());
    AllocaInst* iterableAlloca =
        createEntryBlockAlloca(func, "iterable.alloca", structType);
    ctx.builder->CreateStore(iterableObj, iterableAlloca);
    iterableObj = iterableAlloca;
  }

  // The container object is passed to next()
  Value* containerObj = iterableObj;

  // Class name for method lookup (includes hash prefix for imported types)
  auto iterableClassType = sun::requireTypePtr<sun::ClassType>(
      iterableType, "for-in iterable (needs a next() method)",
      expr.getIterable()->getLocation());
  std::string iterableTypeName = iterableClassType->getMangledName();

  // The iterable is either the iterator itself (has next()) or a container
  // whose iter() produces one
  Function* nextFunc =
      functions().findClassMethod(iterableClassType, iterableTypeName, "next");

  Value* iteratorObj = iterableObj;
  std::shared_ptr<sun::ClassType> iteratorClassType = iterableClassType;

  if (!nextFunc) {
    Function* iterFunc =
        functions().findClassMethod(iterableClassType, iterableTypeName, "iter");
    if (!iterFunc) {
      logAndThrowError("for-in loop: " + iterableTypeName +
                       " must have a next() method or an iter() method");
      return nullptr;
    }

    Value* actualIterator = ctx.builder->CreateCall(
        iterFunc,
        {materializeMethodClosure(iterFunc, iterableObj, "iter.closure")},
        "iter.result");

    llvm::Type* iterRetType = iterFunc->getReturnType();
    if (!iterRetType->isStructTy()) {
      logAndThrowError("iter() must return a class type with a next() method");
      return nullptr;
    }
    AllocaInst* iterAlloca = createEntryBlockAlloca(
        func, "iter.alloca", cast<StructType>(iterRetType));
    ctx.builder->CreateStore(actualIterator, iterAlloca);
    iteratorObj = iterAlloca;

    // The iterator's sun type comes from iter()'s return type (carries the
    // correct hash prefix, unlike LLVM struct names)
    const auto* iterMethod = iterableClassType->getMethod("iter");
    iteratorClassType =
        iterMethod ? sun::tryGetTypePtr<sun::ClassType>(iterMethod->returnType)
                   : nullptr;
    if (!iteratorClassType) {
      logAndThrowError("iter() must return a class type with a next() method");
      return nullptr;
    }
    nextFunc = functions().findClassMethod(iteratorClassType,
                               iteratorClassType->getMangledName(), "next");
    if (!nextFunc) {
      logAndThrowError("Iterator returned by iter() must have next() method");
      return nullptr;
    }
  }

  // next() returns Option<T>: sema verified the shape and that T matches the
  // loop variable annotation
  const auto* nextMethod = iteratorClassType->getMethod("next");
  auto optionType = nextMethod ? sun::tryGetTypePtr<sun::EnumType>(
                                     sun::unwrapRef(nextMethod->returnType))
                               : nullptr;
  if (!optionType || !optionType->getVariant("Some") ||
      !optionType->getVariant("None")) {
    logAndThrowError("for-in loop: next() must return Option<T>");
    return nullptr;
  }
  StructType* optionStorageTy = typeResolver.getEnumStorageType(*optionType);
  StructType* someTy = typeResolver.getEnumVariantStruct(*optionType, "Some");
  unsigned payloadIdx =
      typeResolver.enumPayloadFieldIndex(*optionType, "Some", 0);
  int64_t someTag = optionType->getVariant("Some")->value;

  // Loop variable (type from semantic analysis)
  if (!expr.hasResolvedLoopVarType()) {
    logAndThrowError(
        "Internal error: for-in loop variable type not resolved by semantic "
        "analysis");
    return nullptr;
  }
  sun::TypePtr loopVarType = expr.getResolvedLoopVarType();
  Type* llvmLoopVarType = typeResolver.resolve(loopVarType);
  AllocaInst* loopVarAlloca =
      createEntryBlockAlloca(func, expr.getLoopVar(), llvmLoopVarType);
  scopes().back().variables[expr.getLoopVar()] = loopVarAlloca;
  debugDeclareLocal(loopVarAlloca, expr.getLoopVar(), loopVarType,
                    expr.getLocation());

  // Storage for the Option<T> yielded by each next() call. The payload is
  // copied out into the loop variable; the Option itself is a transient view
  // and is never dropped here.
  AllocaInst* nextAlloca =
      createEntryBlockAlloca(func, "forin.next", optionStorageTy);

  BasicBlock* condBB = BasicBlock::Create(ctx.getContext(), "forin_cond", func);
  BasicBlock* bodyBB = BasicBlock::Create(ctx.getContext(), "forin_body", func);
  BasicBlock* afterBB =
      BasicBlock::Create(ctx.getContext(), "forin_after", func);

  ctx.builder->CreateBr(condBB);

  // Condition: call next(), continue while the tag is Some
  ctx.builder->SetInsertPoint(condBB);
  Value* nextClosure =
      materializeMethodClosure(nextFunc, iteratorObj, "next.closure");
  // next(ref Container): sema verified Container is the iterable's type
  Value* nextResult =
      ctx.builder->CreateCall(nextFunc, {nextClosure, containerObj}, "nextval");
  if (nextResult->getType()->isPointerTy()) {
    nextResult =
        ctx.builder->CreateLoad(optionStorageTy, nextResult, "nextval.load");
  }
  ctx.builder->CreateStore(nextResult, nextAlloca);
  Value* tagPtr = ctx.builder->CreateStructGEP(optionStorageTy, nextAlloca, 0,
                                               "forin.tag.ptr");
  Value* tag = ctx.builder->CreateLoad(Type::getInt32Ty(ctx.getContext()),
                                       tagPtr, "forin.tag");
  Value* isSome = ctx.builder->CreateICmpEQ(
      tag, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), someTag),
      "forin.some");
  ctx.builder->CreateCondBr(isSome, bodyBB, afterBB);

  // Body: bind the payload, then run the user block
  ctx.builder->SetInsertPoint(bodyBB);

  // Per-iteration scope: owners declared in the body are dropped at the
  // back-edge (and by break/continue)
  scopes().push(expr.getBody()->getLocation());
  loopStack.push_back({condBB, afterBB, scopes().size() - 1});

  Value* payloadPtr = ctx.builder->CreateStructGEP(someTy, nextAlloca,
                                                   payloadIdx, "forin.payload");
  Value* payload = ctx.builder->CreateLoad(someTy->getElementType(payloadIdx),
                                           payloadPtr, "forin.elem");
  ctx.builder->CreateStore(payload, loopVarAlloca);

  codegen(*expr.getBody());

  loopStack.pop_back();

  // Emits this iteration's drops unless the block already terminated
  scopes().pop();

  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    ctx.builder->CreateBr(condBB);
  }

  ctx.builder->SetInsertPoint(afterBB);

  // Pop the loop scope (emits cleanup for loop-scoped owners in afterBB)
  scopes().pop();

  // for-in expr always returns 0.0
  return Constant::getNullValue(Type::getDoubleTy(ctx.getContext()));
}
