// loops.cpp - Loop codegen (for loops, while loops, break, continue)

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

Value* CodegenVisitor::codegen(const ForExprAST& expr) {
  Function* func = ctx.builder->GetInsertBlock()->getParent();

  // Enter a new scope for loop variables
  pushScope();

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

  // Push loop context for break/continue (continue goes to step, break goes to
  // after)
  loopStack.push_back({stepBB, afterBB});

  // Emit the body of the loop.
  codegen(*expr.getBody());

  // Pop loop context
  loopStack.pop_back();

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
  popScope();

  // for expr always returns 0.0.
  return Constant::getNullValue(Type::getDoubleTy(ctx.getContext()));
}

Value* CodegenVisitor::codegen(const WhileExprAST& expr) {
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

  // Push loop context for break/continue (continue goes to cond, break goes to
  // after)
  loopStack.push_back({condBB, afterBB});

  // Emit the body of the loop.
  codegen(*expr.getBody());

  // Pop loop context
  loopStack.pop_back();

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

Value* CodegenVisitor::codegen(const BreakAST& expr) {
  if (loopStack.empty()) {
    logAndThrowError("'break' statement not within a loop");
    return nullptr;
  }

  // Jump to the break target (the block after the loop)
  ctx.builder->CreateBr(loopStack.back().breakBlock);

  // Return a dummy value (the branch is the important part)
  return Constant::getNullValue(Type::getDoubleTy(ctx.getContext()));
}

Value* CodegenVisitor::codegen(const ContinueAST& expr) {
  if (loopStack.empty()) {
    logAndThrowError("'continue' statement not within a loop");
    return nullptr;
  }

  // Jump to the continue target (condition for while, step for for)
  ctx.builder->CreateBr(loopStack.back().continueBlock);

  // Return a dummy value (the branch is the important part)
  return Constant::getNullValue(Type::getDoubleTy(ctx.getContext()));
}

Value* CodegenVisitor::codegen(const ForInExprAST& expr) {
  Function* func = ctx.builder->GetInsertBlock()->getParent();

  // Enter a new scope for loop variables
  pushScope();

  // Evaluate the iterable expression to get the container/iterator
  // The iterable is expected to have hasNext(ref Container) -> bool and
  // next(ref Container) -> T methods
  Value* iterableObj = codegen(*expr.getIterable());
  if (!iterableObj) return nullptr;

  // Get the iterable type (should be a struct/class pointer)
  auto iterableType = expr.getIterable()->getResolvedType();
  if (!iterableType) {
    logAndThrowError("Could not determine type of iterable in for-in loop");
    return nullptr;
  }

  // If iterable is a struct value (e.g., from a method call returning by
  // value), we need to store it in an alloca so that it can be passed by ref.
  if (iterableObj->getType()->isStructTy()) {
    llvm::StructType* structType = cast<StructType>(iterableObj->getType());
    AllocaInst* iterableAlloca =
        createEntryBlockAlloca(func, "iterable.alloca", structType);
    ctx.builder->CreateStore(iterableObj, iterableAlloca);
    iterableObj = iterableAlloca;
  }

  // Keep track of the container object for passing to hasNext/next
  Value* containerObj = iterableObj;

  // Get the iterable class name for method lookup (includes hash prefix for
  // imported types, e.g., "$ac9ed853$_sun_Vec_i64")
  std::string iterableTypeName;
  std::shared_ptr<sun::ClassType> iterableClassType;
  if (auto ct = std::dynamic_pointer_cast<sun::ClassType>(iterableType)) {
    iterableTypeName = ct->getName();
    iterableClassType = ct;
  } else {
    logAndThrowError(
        "Iterator in for-in loop must be a class type with hasNext() method");
    return nullptr;
  }

  // Check if the iterable has hasNext() directly, or if we need to call iter()
  std::string hasNextMethodName = iterableTypeName + "_hasNext";
  Function* hasNextFunc = ctx.mainModule->getFunction(hasNextMethodName);

  Value* iteratorObj = iterableObj;
  std::string iteratorTypeName = iterableTypeName;

  if (!hasNextFunc) {
    // No hasNext() - check if it has an iter() method to get an iterator
    std::string iterMethodName = iterableTypeName + "_iter";
    Function* iterFunc = ctx.mainModule->getFunction(iterMethodName);

    if (!iterFunc) {
      logAndThrowError(
          "for-in loop: " + iterableTypeName +
          " must have hasNext()/next() methods or an iter() method");
      return nullptr;
    }

    // Call iter() to get the actual iterator
    Value* actualIterator =
        ctx.builder->CreateCall(iterFunc, {iterableObj}, "iter.result");

    // Get the iterator type from iter()'s return type
    llvm::Type* iterRetType = iterFunc->getReturnType();
    if (!iterRetType->isStructTy()) {
      logAndThrowError(
          "iter() must return a class type with hasNext()/next() methods");
      return nullptr;
    }

    // Store the iterator struct in an alloca
    llvm::StructType* iterStructType = cast<StructType>(iterRetType);
    AllocaInst* iterAlloca =
        createEntryBlockAlloca(func, "iter.alloca", iterStructType);
    ctx.builder->CreateStore(actualIterator, iterAlloca);
    iteratorObj = iterAlloca;

    // Get the iterator type name from the iter() method's return type in
    // the sun type system (has the correct hash prefix, unlike LLVM struct
    // names)
    const auto* iterMethod = iterableClassType->getMethod("iter");
    if (iterMethod && iterMethod->returnType &&
        iterMethod->returnType->isClass()) {
      iteratorTypeName =
          static_cast<const sun::ClassType*>(iterMethod->returnType.get())
              ->getName();
    } else {
      // Fallback: extract from LLVM struct name
      StringRef structName = iterStructType->getName();
      if (structName.ends_with("_struct")) {
        iteratorTypeName = structName.drop_back(7).str();
      } else {
        iteratorTypeName = structName.str();
      }
    }

    // Now look up hasNext on the actual iterator type
    hasNextMethodName = iteratorTypeName + "_hasNext";
    hasNextFunc = ctx.mainModule->getFunction(hasNextMethodName);
    if (!hasNextFunc) {
      logAndThrowError(
          "Iterator returned by iter() must have hasNext() method");
      return nullptr;
    }
  }

  // Get the loop variable type from semantic analysis
  if (!expr.hasResolvedLoopVarType()) {
    logAndThrowError(
        "Internal error: for-in loop variable type not resolved by semantic "
        "analysis");
    return nullptr;
  }
  sun::TypePtr loopVarType = expr.getResolvedLoopVarType();

  // Resolve LLVM type for the loop variable
  Type* llvmLoopVarType = typeResolver.resolve(loopVarType);

  // Create the loop variable alloca
  AllocaInst* loopVarAlloca =
      createEntryBlockAlloca(func, expr.getLoopVar(), llvmLoopVarType);

  // Register the loop variable in the current scope
  scopes.back().variables[expr.getLoopVar()] = loopVarAlloca;

  // Create basic blocks for the loop structure
  BasicBlock* condBB = BasicBlock::Create(ctx.getContext(), "forin_cond", func);
  BasicBlock* bodyBB = BasicBlock::Create(ctx.getContext(), "forin_body", func);
  BasicBlock* afterBB =
      BasicBlock::Create(ctx.getContext(), "forin_after", func);

  // Branch to the condition check first
  ctx.builder->CreateBr(condBB);

  // Emit condition check block (call hasNext())
  ctx.builder->SetInsertPoint(condBB);

  // Determine if hasNext takes a container ref parameter (new style) or not
  // (old duck-typed style) by checking the function argument count
  bool hasNextTakesContainerRef = hasNextFunc->arg_size() == 2;

  // Call hasNext with appropriate arguments
  Value* hasNextResult;
  if (hasNextTakesContainerRef) {
    hasNextResult = ctx.builder->CreateCall(
        hasNextFunc, {iteratorObj, containerObj}, "hasnext");
  } else {
    hasNextResult =
        ctx.builder->CreateCall(hasNextFunc, {iteratorObj}, "hasnext");
  }

  // Branch: if hasNext() returns true go to body, else exit
  ctx.builder->CreateCondBr(hasNextResult, bodyBB, afterBB);

  // Emit loop body
  ctx.builder->SetInsertPoint(bodyBB);

  // Push loop context for break/continue
  loopStack.push_back({condBB, afterBB});

  // Call iterator.next() and store result in loop variable
  std::string nextMethodName = iteratorTypeName + "_next";
  Function* nextFunc = ctx.mainModule->getFunction(nextMethodName);
  if (!nextFunc) {
    logAndThrowError("for-in loop iterator must have next() method");
    return nullptr;
  }

  // Determine if next takes a container ref parameter (new style) or not
  // (old duck-typed style) by checking the function argument count
  bool nextTakesContainerRef = nextFunc->arg_size() == 2;

  // Call next() with appropriate arguments to get the value
  Value* nextResult;
  if (nextTakesContainerRef) {
    nextResult = ctx.builder->CreateCall(nextFunc, {iteratorObj, containerObj},
                                         "nextval");
  } else {
    nextResult = ctx.builder->CreateCall(nextFunc, {iteratorObj}, "nextval");
  }

  // Store the result in the loop variable
  ctx.builder->CreateStore(nextResult, loopVarAlloca);

  // Emit the body of the loop
  codegen(*expr.getBody());

  // Pop loop context
  loopStack.pop_back();

  // Only emit branch to condition if current block has no terminator
  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    ctx.builder->CreateBr(condBB);
  }

  // Continue inserting after the loop
  ctx.builder->SetInsertPoint(afterBB);

  // Pop the loop scope
  popScope();

  // for-in expr always returns 0.0
  return Constant::getNullValue(Type::getDoubleTy(ctx.getContext()));
}
