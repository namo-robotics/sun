// error_handling.cpp — Codegen for error handling expressions (try/catch/throw)

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include "codegen_visitor.h"

using namespace llvm;

// Safe division: check for zero divisor and return error instead of crashing
// This is only called when currentFunctionCanError is true
Value* CodegenVisitor::codegenSafeDivision(Value* L, Value* R) {
  // Get current function and its return type
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  llvm::Type* retType = func->getReturnType();

  // Check if R == 0
  Value* isZero = ctx.builder->CreateICmpEQ(
      R, ConstantInt::get(R->getType(), 0), "div.zero.check");

  // Create blocks for the check
  BasicBlock* currentBB = ctx.builder->GetInsertBlock();
  BasicBlock* zeroBB = BasicBlock::Create(ctx.getContext(), "div.zero", func);
  BasicBlock* safeBB = BasicBlock::Create(ctx.getContext(), "div.safe", func);
  BasicBlock* mergeBB = BasicBlock::Create(ctx.getContext(), "div.merge", func);

  ctx.builder->CreateCondBr(isZero, zeroBB, safeBB);

  // Zero case: return error
  ctx.builder->SetInsertPoint(zeroBB);
  Value* errorResult = UndefValue::get(retType);
  errorResult = ctx.builder->CreateInsertValue(
      errorResult, ConstantInt::getTrue(ctx.getContext()), 0, "error.flag");
  Value* zeroVal = Constant::getNullValue(currentFunctionValueType);
  errorResult =
      ctx.builder->CreateInsertValue(errorResult, zeroVal, 1, "error.value");
  ctx.builder->CreateRet(errorResult);

  // Safe case: perform division and continue
  ctx.builder->SetInsertPoint(safeBB);
  Value* divResult = ctx.builder->CreateSDiv(L, R, "divtmp");
  ctx.builder->CreateBr(mergeBB);

  // Merge block: continue with the division result
  ctx.builder->SetInsertPoint(mergeBB);

  // Return the division result for use in subsequent code
  // (the error case already returned from the function)
  return divResult;
}

// Codegen for throw expression: throw <expr>
// Creates an error union with isError = true and returns it from the function
Value* CodegenVisitor::codegen(const ThrowExprAST& expr) {
  // We must be in a function that can return errors
  if (!currentFunctionCanError) {
    logAndThrowError(
        "throw can only be used in functions declared with ', IError'");
    return nullptr;
  }

  // Get the function's return type (which is the error union struct)
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  llvm::Type* retType = func->getReturnType();

  // Generate code for the error expression (for now we ignore the value
  // and just set the error flag - future: store error info)
  if (expr.hasErrorExpr()) {
    codegen(expr.getErrorExpr());  // Evaluate for side effects
  }

  // Create error union: { i1 isError = true, T value = undef }
  Value* errorStruct = UndefValue::get(retType);
  errorStruct = ctx.builder->CreateInsertValue(
      errorStruct, ConstantInt::getTrue(ctx.getContext()), 0, "error.flag");

  // Value field is undefined for errors - use zero value
  if (currentFunctionValueType) {
    Value* zeroVal = Constant::getNullValue(currentFunctionValueType);
    errorStruct =
        ctx.builder->CreateInsertValue(errorStruct, zeroVal, 1, "error.value");
  }

  // Return the error union immediately
  ctx.builder->CreateRet(errorStruct);

  // Create an unreachable block for any subsequent code in this control flow
  // path Add an unreachable instruction as terminator so LLVM knows this is
  // dead code
  BasicBlock* unreachableBB =
      BasicBlock::Create(ctx.getContext(), "throw.unreachable", func);
  ctx.builder->SetInsertPoint(unreachableBB);
  ctx.builder->CreateUnreachable();

  // Return nullptr to indicate this expression terminates (no usable value)
  return nullptr;
}

// Codegen for try-catch expression: try { ... } catch (e: IError) { ... }
// Errors can be caught at any point during the try block (not just at the end)
Value* CodegenVisitor::codegen(const TryCatchExprAST& expr) {
  Function* func = ctx.builder->GetInsertBlock()->getParent();

  // Create an alloca to store any error result from calls in the try block
  // Use a generic error union type: { i1, i32 }
  llvm::Type* errorUnionType = StructType::get(
      ctx.getContext(),
      {Type::getInt1Ty(ctx.getContext()), Type::getInt32Ty(ctx.getContext())});
  IRBuilder<> entryBuilder(&func->getEntryBlock(),
                           func->getEntryBlock().begin());
  AllocaInst* errorResultAlloca =
      entryBuilder.CreateAlloca(errorUnionType, nullptr, "try.error.result");

  // Create basic blocks
  BasicBlock* catchBB = BasicBlock::Create(ctx.getContext(), "try.catch", func);
  BasicBlock* successBB =
      BasicBlock::Create(ctx.getContext(), "try.success", func);
  BasicBlock* mergeBB = BasicBlock::Create(ctx.getContext(), "try.merge", func);

  // Push try context so function calls can jump to catch on error
  tryStack.push_back({catchBB, errorResultAlloca});

  // Generate code for the try block
  pushScope();
  Value* tryResult = codegen(expr.getTryBlock());
  popScope();

  // Pop try context
  tryStack.pop_back();

  // Determine the value type for the try/catch result
  llvm::Type* valueType = Type::getInt32Ty(ctx.getContext());

  // If we reached the end of the try block without jumping to catch,
  // branch to success (only if the current block has no terminator)
  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    // Check if the try result itself is an error union (from a direct call at
    // the end)
    if (tryResult && tryResult->getType()->isStructTy()) {
      auto* structType = dyn_cast<StructType>(tryResult->getType());
      if (structType && structType->getNumElements() == 2 &&
          structType->getElementType(0)->isIntegerTy(1)) {
        // Final expression was an error union - check it
        Value* isError =
            ctx.builder->CreateExtractValue(tryResult, 0, "final.isError");
        ctx.builder->CreateStore(tryResult, errorResultAlloca);
        ctx.builder->CreateCondBr(isError, catchBB, successBB);
        valueType = structType->getElementType(1);
      } else {
        ctx.builder->CreateBr(successBB);
        if (tryResult) valueType = tryResult->getType();
      }
    } else {
      ctx.builder->CreateBr(successBB);
      if (tryResult) valueType = tryResult->getType();
    }
  }

  // Track results for phi node
  std::vector<std::pair<Value*, BasicBlock*>> results;

  // Generate catch block
  ctx.builder->SetInsertPoint(catchBB);
  pushScope();

  const auto& catchClause = expr.getCatchClause();
  if (!catchClause.bindingName.empty()) {
    // Bind the error to a variable
    IRBuilder<> tmpBuilder(&func->getEntryBlock(),
                           func->getEntryBlock().begin());
    AllocaInst* alloca = tmpBuilder.CreateAlloca(
        Type::getInt64Ty(ctx.getContext()), nullptr, catchClause.bindingName);

    // Load the error result and extract error value
    Value* storedError = ctx.builder->CreateLoad(
        errorUnionType, errorResultAlloca, "error.loaded");
    Value* errorVal =
        ctx.builder->CreateExtractValue(storedError, 1, "error.val");
    if (errorVal->getType()->isIntegerTy()) {
      if (errorVal->getType() != Type::getInt64Ty(ctx.getContext())) {
        errorVal = ctx.builder->CreateSExtOrTrunc(
            errorVal, Type::getInt64Ty(ctx.getContext()));
      }
      ctx.builder->CreateStore(errorVal, alloca);
    } else {
      ctx.builder->CreateStore(
          ConstantInt::get(Type::getInt64Ty(ctx.getContext()), 1), alloca);
    }
    scopes.back().variables[catchClause.bindingName] = alloca;
  }

  Value* catchResult = codegen(*catchClause.body);
  popScope();

  // Convert catch result to match value type if needed (before the branch)
  Value* catchValue = catchResult;
  if (catchResult && !ctx.builder->GetInsertBlock()->getTerminator()) {
    if (catchResult->getType() != valueType) {
      // Convert catch result to match the expected value type
      if (valueType->isIntegerTy() && catchResult->getType()->isIntegerTy()) {
        catchValue = ctx.builder->CreateSExtOrTrunc(catchResult, valueType,
                                                    "catch.convert");
      } else if (valueType->isIntegerTy() &&
                 catchResult->getType()->isFloatingPointTy()) {
        catchValue =
            ctx.builder->CreateFPToSI(catchResult, valueType, "catch.convert");
      } else if (valueType->isFloatingPointTy() &&
                 catchResult->getType()->isIntegerTy()) {
        catchValue =
            ctx.builder->CreateSIToFP(catchResult, valueType, "catch.convert");
      } else if (valueType->isFloatingPointTy() &&
                 catchResult->getType()->isFloatingPointTy()) {
        catchValue =
            ctx.builder->CreateFPCast(catchResult, valueType, "catch.convert");
      }
    }
    ctx.builder->CreateBr(mergeBB);
    results.push_back({catchValue, ctx.builder->GetInsertBlock()});
  }

  // Generate success block
  ctx.builder->SetInsertPoint(successBB);

  // Get the success value (either from unwrapped try result or direct value)
  Value* successValue = tryResult;
  if (tryResult && tryResult->getType()->isStructTy()) {
    auto* structType = dyn_cast<StructType>(tryResult->getType());
    if (structType && structType->getNumElements() == 2 &&
        structType->getElementType(0)->isIntegerTy(1)) {
      // Extract the value from the error union struct
      successValue = ctx.builder->CreateExtractValue(tryResult, 1, "value");
    }
  }

  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    if (successValue && successValue->getType() != valueType) {
      // Convert success value to match expected type
      if (valueType->isIntegerTy() && successValue->getType()->isIntegerTy()) {
        successValue = ctx.builder->CreateSExtOrTrunc(successValue, valueType,
                                                      "success.convert");
      }
    }
    ctx.builder->CreateBr(mergeBB);
    if (successValue) {
      results.push_back({successValue, ctx.builder->GetInsertBlock()});
    }
  }

  // Set up merge block
  ctx.builder->SetInsertPoint(mergeBB);

  // If there are results, create a phi node
  if (!results.empty()) {
    PHINode* phi =
        ctx.builder->CreatePHI(valueType, results.size(), "try.result");
    for (auto& [val, block] : results) {
      phi->addIncoming(val, block);
    }
    return phi;
  }

  // No results (all branches return or have void type)
  return Constant::getNullValue(valueType);
}
