// block_expressions.cpp - Block expression codegen

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"

using namespace llvm;

// A block is a scope: it declares what it defines before emitting any body,
// then drops whatever it still owns on the way out.
Value* CodegenVisitor::codegen(const BlockExprAST& block) {
  if (block.isEmpty()) return ConstantFP::get(ctx.getContext(), APFloat(0.0));

  functions_.declareBlockSignatures(block);

  Value* lastValue = nullptr;
  bool encounteredReturn = false;

  for (const auto& expr : block.getBody()) {
    if (encounteredReturn) {
      // Code after return is unreachable, skip codegen
      break;
    }

    if (expr->isFunction()) {
      // Save current insertion point before generating function
      auto currentBlock = ctx.builder->GetInsertBlock();

      auto& funcAST = static_cast<FunctionAST&>(*expr);
      Value* funcVal = functions_.codegenFunc(funcAST);

      // Restore!
      if (currentBlock) {
        ctx.builder->SetInsertPoint(currentBlock);
      }

      // For nested named functions with closures, store the env pointer in
      // local scope so calls can pass it as the first argument. Keyed by the
      // symbol the function is emitted under — a specialization is called by
      // its mangled name, never by the template's.
      const auto& proto = funcAST.getProto();
      if (!scopes.empty() && !proto.getName().empty() && proto.hasClosure()) {
        // funcVal is an env pointer - store it in scope
        if (funcVal) {
          scopes.back().variables[proto.getMangledName()] =
              llvm::cast<llvm::AllocaInst>(funcVal);
        }
      }

      continue;
    }

    // Class/interface definitions generate functions/methods that change
    // the IR builder insertion point. Save/restore around them.
    if (expr->getType() == ASTNodeType::CLASS_DEFINITION ||
        expr->getType() == ASTNodeType::INTERFACE_DEFINITION) {
      auto currentBlock = ctx.builder->GetInsertBlock();

      lastValue = codegen(*expr);

      if (currentBlock) {
        ctx.builder->SetInsertPoint(currentBlock);
      }
      continue;
    }

    if (expr->isReturn()) {
      // Generate the return - this will terminate the current basic block
      codegen(*expr);
      encounteredReturn = true;
      continue;
    }

    lastValue = codegen(*expr);

    // Check if the expression we just generated terminated the block (e.g., if
    // with returns in both branches)
    // Only do this check when we're inside a function (scopes non-empty),
    // not at top-level where there's no meaningful "current block"
    if (!scopes.empty()) {
      auto* insertBlock = ctx.builder->GetInsertBlock();
      if (insertBlock != nullptr && insertBlock->getTerminator() != nullptr) {
        // Block was terminated by this expression (e.g., if statement where
        // both branches return)
        encounteredReturn = true;
        continue;
      }
    }

    if (!lastValue) {
      return nullptr;
    }
  }

  // If we encountered a return, the block has already been terminated
  // Return a dummy value - the caller will check for terminator
  if (encounteredReturn) {
    // Return a poison value to indicate block was terminated by return
    // This won't be used since the function has already returned
    return nullptr;
  }

  // If the block ended with declarations only → return 0 (or whatever your
  // language semantics want)
  return lastValue ? lastValue
                   : ConstantFP::get(ctx.getContext(), APFloat(0.0));
}
