// if_expressions.cpp - If expression codegen

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

Value* CodegenVisitor::codegen(const IfExprAST& expr) {
  Value* CondV = codegen(*expr.getCond());
  if (!CondV) return nullptr;

  // Convert condition to a bool if not already i1
  if (!CondV->getType()->isIntegerTy(1)) {
    if (CondV->getType()->isFloatingPointTy()) {
      // Float: compare non-equal to 0.0
      CondV = ctx.builder->CreateFCmpONE(
          CondV, ConstantFP::get(CondV->getType(), 0.0), "ifcond");
    } else if (CondV->getType()->isIntegerTy()) {
      // Integer: compare non-equal to 0
      CondV = ctx.builder->CreateICmpNE(
          CondV, ConstantInt::get(CondV->getType(), 0), "ifcond");
    }
  }

  Function* TheFunction = ctx.builder->GetInsertBlock()->getParent();
  bool hasElse = expr.getElse() != nullptr;

  // Create blocks for the then and else cases.  Insert the 'then' block at the
  // end of the function.
  BasicBlock* ThenBB =
      BasicBlock::Create(ctx.getContext(), "then", TheFunction);
  BasicBlock* ElseBB =
      hasElse ? BasicBlock::Create(ctx.getContext(), "else") : nullptr;
  // MergeBB must be inserted into the function before creating the conditional
  // branch so LLVM properly tracks the CFG edges
  BasicBlock* MergeBB =
      BasicBlock::Create(ctx.getContext(), "ifcont", TheFunction);

  // Branch to then or else (or merge if no else)
  ctx.builder->CreateCondBr(CondV, ThenBB, hasElse ? ElseBB : MergeBB);

  // Emit then value.
  ctx.builder->SetInsertPoint(ThenBB);

  // Push scope for then block (variables declared here are local to this block)
  pushScope();
  Value* ThenV = codegen(*expr.getThen());
  popScope();

  // Check if the Then block was terminated (e.g., by a return statement)
  bool thenTerminated =
      ctx.builder->GetInsertBlock()->getTerminator() != nullptr;

  if (!thenTerminated) {
    if (!ThenV) {
      logAndThrowError("Failed to generate code for 'then' block");
      return nullptr;
    }
    ctx.builder->CreateBr(MergeBB);
  }
  // Codegen of 'Then' can change the current block, update ThenBB for the PHI.
  ThenBB = ctx.builder->GetInsertBlock();

  // Handle else block (if present)
  Value* ElseV = nullptr;
  bool elseTerminated = false;

  if (hasElse) {
    TheFunction->insert(TheFunction->end(), ElseBB);
    ctx.builder->SetInsertPoint(ElseBB);

    // Push scope for else block (variables declared here are local to this
    // block)
    pushScope();
    ElseV = codegen(*expr.getElse());
    popScope();

    elseTerminated = ctx.builder->GetInsertBlock()->getTerminator() != nullptr;

    if (!elseTerminated) {
      if (!ElseV) {
        logAndThrowError("Failed to generate code for 'else' block");
        return nullptr;
      }
      ctx.builder->CreateBr(MergeBB);
    }
    ElseBB = ctx.builder->GetInsertBlock();
  }

  // If both branches terminated (returned), we don't need the merge block
  // BUT: if there's no else, MergeBB is the target of the conditional branch's
  // false path so we can only delete it if both then and else explicitly
  // terminated
  if (thenTerminated && elseTerminated) {
    // Remove MergeBB from the function and delete it
    MergeBB->eraseFromParent();
    return nullptr;
  }

  // Set insert point to merge block (it's already in the function)
  ctx.builder->SetInsertPoint(MergeBB);

  // No else block - the if is used as a statement
  if (!hasElse) {
    // Return a dummy value to indicate the if expression completed
    // (the actual return value won't be used since if-without-else is a
    // statement)
    return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
  }

  // If only one branch terminated, return the other's value
  if (thenTerminated || elseTerminated) {
    return thenTerminated ? ElseV : ThenV;
  }

  // Both branches have values - if types differ, don't create PHI
  if (ThenV->getType() != ElseV->getType()) {
    // Different types - return void/unit (if expression used as statement)
    return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
  }

  // Both branches have values with matching types - create PHI node
  PHINode* PN = ctx.builder->CreatePHI(ThenV->getType(), 2, "iftmp");

  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}
