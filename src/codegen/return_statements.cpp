// return_statements.cpp - Return expression codegen

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

Value* CodegenVisitor::codegen(const ReturnExprAST& expr) {
  llvm::Function* func = ctx.builder->GetInsertBlock()->getParent();
  llvm::Type* retType = func->getReturnType();

  if (expr.hasValue()) {
    // Return with a value
    // IMPORTANT: Evaluate return expression FIRST (may transfer ownership)
    Value* retVal = codegen(*expr.getValue());
    if (!retVal) return nullptr;

    // THEN clean up owned allocations that weren't moved (move semantics)
    emitScopeCleanup();

    // If this function can return errors, wrap the value in an error union
    // struct
    if (currentFunctionCanError && currentFunctionValueType) {
      // Check if we're returning an error literal (already an error union)
      if (retVal->getType()->isStructTy() && retVal->getType() == retType) {
        // Already a properly typed error union, just return it
        ctx.builder->CreateRet(retVal);
        return nullptr;
      }

      // Convert return value to match the value type if needed
      if (retVal->getType() != currentFunctionValueType) {
        // For class types: if returning a pointer but value type is struct,
        // load the struct
        if (currentFunctionValueType->isStructTy() &&
            retVal->getType()->isPointerTy()) {
          retVal = ctx.builder->CreateLoad(currentFunctionValueType, retVal,
                                           "struct.load");
        } else if (currentFunctionValueType->isIntegerTy() &&
                   retVal->getType()->isIntegerTy()) {
          retVal = ctx.builder->CreateIntCast(retVal, currentFunctionValueType,
                                              true, "intcast");
        } else if (currentFunctionValueType->isFloatingPointTy() &&
                   retVal->getType()->isIntegerTy()) {
          retVal = ctx.builder->CreateSIToFP(retVal, currentFunctionValueType,
                                             "inttofp");
        } else if (currentFunctionValueType->isIntegerTy() &&
                   retVal->getType()->isFloatingPointTy()) {
          retVal = ctx.builder->CreateFPToSI(retVal, currentFunctionValueType,
                                             "fptoint");
        } else if (currentFunctionValueType->isFloatingPointTy() &&
                   retVal->getType()->isFloatingPointTy()) {
          retVal = ctx.builder->CreateFPCast(retVal, currentFunctionValueType,
                                             "fpcast");
        }
      }

      // Create error union: { i1 isError = false, T value }
      Value* errorUnion = UndefValue::get(retType);
      errorUnion = ctx.builder->CreateInsertValue(
          errorUnion, ConstantInt::getFalse(ctx.getContext()), 0,
          "success.flag");
      errorUnion = ctx.builder->CreateInsertValue(errorUnion, retVal, 1,
                                                  "success.value");
      ctx.builder->CreateRet(errorUnion);
      return nullptr;
    }

    // Convert return value to match function return type if needed
    if (retVal->getType() != retType) {
      // Handle return-by-value for compound types (classes):
      // If the function returns a struct type but we have a pointer,
      // load the struct from the pointer to return it by value.
      if (retType->isStructTy() && retVal->getType()->isPointerTy()) {
        retVal = ctx.builder->CreateLoad(retType, retVal, "struct.ret");
      } else if (retType->isDoubleTy() && retVal->getType()->isIntegerTy()) {
        retVal = ctx.builder->CreateSIToFP(retVal, retType, "inttofp");
      } else if (retType->isDoubleTy() && retVal->getType()->isFloatTy()) {
        retVal = ctx.builder->CreateFPExt(retVal, retType, "fpext");
      } else if (retType->isIntegerTy() &&
                 retVal->getType()->isFloatingPointTy()) {
        retVal = ctx.builder->CreateFPToSI(retVal, retType, "fptoint");
      } else if (retType->isIntegerTy() && retVal->getType()->isIntegerTy()) {
        retVal = ctx.builder->CreateIntCast(retVal, retType, true, "intcast");
      } else if (retType->isFloatingPointTy() &&
                 retVal->getType()->isFloatingPointTy()) {
        retVal = ctx.builder->CreateFPCast(retVal, retType, "fpcast");
      }
    }

    ctx.builder->CreateRet(retVal);
  } else {
    // Void return - clean up and return
    emitScopeCleanup();
    ctx.builder->CreateRetVoid();
  }

  // Return nullptr to indicate this expression doesn't produce a value
  // (the function has already been terminated)
  // The current basic block now has a terminator, so any subsequent code
  // in this block will be unreachable.
  return nullptr;
}
