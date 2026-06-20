// error_handling.cpp — Codegen for error handling expressions (try/catch/throw)

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include "codegen_visitor.h"

using namespace llvm;

// Safe division/modulo: check for zero divisor and return error instead of crashing
// This is only called when currentFunctionCanError is true
Value* CodegenVisitor::codegenSafeDivision(Value* L, Value* R, bool isModulo) {
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

  // Safe case: perform division or modulo and continue
  ctx.builder->SetInsertPoint(safeBB);
  Value* result = isModulo ? ctx.builder->CreateSRem(L, R, "modtmp")
                           : ctx.builder->CreateSDiv(L, R, "divtmp");
  ctx.builder->CreateBr(mergeBB);

  // Merge block: continue with the result
  ctx.builder->SetInsertPoint(mergeBB);

  // Return the result for use in subsequent code
  // (the error case already returned from the function)
  return result;
}

// Short-circuit logical operators (and, or)
// 'and' evaluates to: LHS ? RHS : false
// 'or' evaluates to: LHS ? true : RHS
Value* CodegenVisitor::codegenLogicalOp(const BinaryExprAST& expr) {
  bool isAnd = (expr.getOp().kind == TokenKind::AND);

  // Evaluate LHS
  Value* L = codegen(*expr.getLHS());
  if (!L) return nullptr;

  // Convert LHS to bool (i1) if not already
  if (!L->getType()->isIntegerTy(1)) {
    if (L->getType()->isFloatingPointTy()) {
      L = ctx.builder->CreateFCmpONE(L, ConstantFP::get(L->getType(), 0.0),
                                      "tobool");
    } else if (L->getType()->isIntegerTy()) {
      L = ctx.builder->CreateICmpNE(L, ConstantInt::get(L->getType(), 0),
                                     "tobool");
    } else {
      logAndThrowError("Logical operator requires boolean-compatible operand",
                       expr.getLocation());
    }
  }

  Function* TheFunction = ctx.builder->GetInsertBlock()->getParent();

  // For 'and': if LHS is false, result is false (don't evaluate RHS)
  // For 'or': if LHS is true, result is true (don't evaluate RHS)
  BasicBlock* EvalRhsBB =
      BasicBlock::Create(ctx.getContext(), isAnd ? "and.rhs" : "or.rhs", TheFunction);
  BasicBlock* MergeBB =
      BasicBlock::Create(ctx.getContext(), isAnd ? "and.end" : "or.end", TheFunction);

  // Remember the block where LHS was evaluated (for PHI)
  BasicBlock* LhsBB = ctx.builder->GetInsertBlock();

  // For 'and': branch to RHS evaluation if LHS is true, otherwise short-circuit to merge
  // For 'or': branch to RHS evaluation if LHS is false, otherwise short-circuit to merge
  if (isAnd) {
    ctx.builder->CreateCondBr(L, EvalRhsBB, MergeBB);
  } else {
    ctx.builder->CreateCondBr(L, MergeBB, EvalRhsBB);
  }

  // Evaluate RHS
  ctx.builder->SetInsertPoint(EvalRhsBB);
  Value* R = codegen(*expr.getRHS());
  if (!R) return nullptr;

  // Convert RHS to bool (i1) if not already
  if (!R->getType()->isIntegerTy(1)) {
    if (R->getType()->isFloatingPointTy()) {
      R = ctx.builder->CreateFCmpONE(R, ConstantFP::get(R->getType(), 0.0),
                                      "tobool");
    } else if (R->getType()->isIntegerTy()) {
      R = ctx.builder->CreateICmpNE(R, ConstantInt::get(R->getType(), 0),
                                     "tobool");
    } else {
      logAndThrowError("Logical operator requires boolean-compatible operand",
                       expr.getLocation());
    }
  }

  // Branch to merge block after evaluating RHS
  ctx.builder->CreateBr(MergeBB);

  // Update RhsBB (codegen of RHS might have changed the current block)
  BasicBlock* RhsBB = ctx.builder->GetInsertBlock();

  // Emit merge block with PHI node
  ctx.builder->SetInsertPoint(MergeBB);
  PHINode* PN = ctx.builder->CreatePHI(Type::getInt1Ty(ctx.getContext()), 2,
                                        isAnd ? "and.result" : "or.result");

  // For 'and': short-circuit value is false, evaluated value is RHS
  // For 'or': short-circuit value is true, evaluated value is RHS
  Value* ShortCircuitVal = isAnd ? ConstantInt::getFalse(ctx.getContext())
                                  : ConstantInt::getTrue(ctx.getContext());
  PN->addIncoming(ShortCircuitVal, LhsBB);
  PN->addIncoming(R, RhsBB);

  return PN;
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

// Codegen for unsafe block: unsafe { ... }
// Simply generates code for the body - safety checks are done at semantic analysis
Value* CodegenVisitor::codegen(const UnsafeBlockAST& expr) {
  return codegen(expr.getBody());
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

// =============================================================================
// LLVM Exception Handling Helpers
// =============================================================================
// These functions declare/get the C++ ABI exception handling functions
// needed for LLVM's native exception handling mechanism.

// Get or declare: void* __cxa_allocate_exception(size_t)
FunctionCallee CodegenVisitor::getCxaAllocateException() {
  auto* i8PtrTy = PointerType::getUnqual(ctx.getContext());
  auto* i64Ty = Type::getInt64Ty(ctx.getContext());
  FunctionType* fnType = FunctionType::get(i8PtrTy, {i64Ty}, false);
  return module->getOrInsertFunction("__cxa_allocate_exception", fnType);
}

// Get or declare: void __cxa_throw(void* exception, void* tinfo, void* dest)
FunctionCallee CodegenVisitor::getCxaThrow() {
  auto* voidTy = Type::getVoidTy(ctx.getContext());
  auto* i8PtrTy = PointerType::getUnqual(ctx.getContext());
  FunctionType* fnType =
      FunctionType::get(voidTy, {i8PtrTy, i8PtrTy, i8PtrTy}, false);
  auto fn = module->getOrInsertFunction("__cxa_throw", fnType);
  // Mark __cxa_throw as noreturn
  if (auto* func = dyn_cast<Function>(fn.getCallee())) {
    func->addFnAttr(Attribute::NoReturn);
  }
  return fn;
}

// Get or declare: void* __cxa_begin_catch(void* exception)
FunctionCallee CodegenVisitor::getCxaBeginCatch() {
  auto* i8PtrTy = PointerType::getUnqual(ctx.getContext());
  FunctionType* fnType = FunctionType::get(i8PtrTy, {i8PtrTy}, false);
  return module->getOrInsertFunction("__cxa_begin_catch", fnType);
}

// Get or declare: void __cxa_end_catch()
FunctionCallee CodegenVisitor::getCxaEndCatch() {
  auto* voidTy = Type::getVoidTy(ctx.getContext());
  FunctionType* fnType = FunctionType::get(voidTy, {}, false);
  return module->getOrInsertFunction("__cxa_end_catch", fnType);
}

// Get the personality function for exception handling
// We use __gxx_personality_v0 for C++ ABI compatibility
Constant* CodegenVisitor::getPersonalityFunction() {
  auto* i32Ty = Type::getInt32Ty(ctx.getContext());
  // Personality function signature: i32 (i32, i32, i64, ptr, ptr)
  // But we just need a function pointer, so declare it minimally
  FunctionType* fnType = FunctionType::get(i32Ty, true);  // varargs
  auto fn = module->getOrInsertFunction("__gxx_personality_v0", fnType);
  return cast<Constant>(fn.getCallee());
}

// Get or create the type info for Sun exceptions
// This is a global constant that identifies our exception type
Constant* CodegenVisitor::getSunExceptionTypeInfo() {
  // Look for existing type info
  if (GlobalVariable* existing =
          module->getGlobalVariable("_ZTIP7IError", true)) {
    return existing;
  }

  // Create a minimal type info structure for IError*
  // In practice, we'll catch all exceptions and check the type ourselves
  // For simplicity, we use a null catch clause (catch-all) in landing pads
  auto* i8PtrTy = PointerType::getUnqual(ctx.getContext());

  // Create external type info reference (will be provided by C++ runtime)
  // For a catch-all, we can use null, but for typed catches we need real RTTI
  // For now, return null pointer to indicate catch-all semantics
  return ConstantPointerNull::get(i8PtrTy);
}
