// return_statements.cpp - Return expression codegen

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

Value* CodegenVisitor::codegen(const ReturnExprAST& expr) {
  llvm::Function* func = ctx.builder->GetInsertBlock()->getParent();
  llvm::Type* retType = func->getReturnType();

  if (expr.hasValue()) {
    // Reference returns must return the referent's ADDRESS, not the
    // auto-dereffed value the normal expression path produces
    if (currentFunctionReturnsRef) {
      Value* addr = tryCodegenAddress(*expr.getValue());
      if (!addr) {
        // Expressions that are themselves reference-typed codegen directly
        // to the address: _to_ref<T>(ptr), and a call forwarding a borrow.
        sun::TypePtr valType = expr.getValue()->getResolvedType();
        if (valType && valType->isReference()) {
          Value* v = codegen(*expr.getValue());
          if (v && v->getType()->isPointerTy()) {
            addr = v;
          }
        }
      }
      if (!addr) {
        logAndThrowError(
            "Cannot return a reference to a temporary or non-addressable "
            "expression",
            expr.getLocation());
      }
      emitScopeCleanup();
      ctx.builder->CreateRet(addr);
      return nullptr;
    }

    // Return with a value
    // IMPORTANT: Evaluate return expression FIRST (may transfer ownership)
    Value* retVal = codegen(*expr.getValue());
    if (!retVal) return nullptr;

    // Move semantics: borrow checker marks expressions as "moved" when
    // ownership transfers (return, assignment, pass-by-value). Skip deinit.
    if (expr.getValue()->isMoved() && retVal) {
      markClassAllocationAsDeinited(retVal);
    }

    // THEN clean up owned allocations that weren't moved (move semantics)
    emitScopeCleanup();

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
    // Void return - clean up and return. A 'void, IError' function returns
    // plain void now (exceptions carry error state, not the return value).
    emitScopeCleanup();
    ctx.builder->CreateRetVoid();
  }

  // Return nullptr to indicate this expression doesn't produce a value
  // (the function has already been terminated)
  // The current basic block now has a terminator, so any subsequent code
  // in this block will be unreachable.
  return nullptr;
}
