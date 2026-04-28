// block_expressions.cpp - Block expression codegen

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

Value* CodegenVisitor::codegen(const BlockExprAST& block) {
  if (block.isEmpty()) return ConstantFP::get(ctx.getContext(), APFloat(0.0));

  // Pre-pass: emit forward declarations for all named functions in this block.
  // This enables mutual recursion: isEven can call isOdd before isOdd is
  // fully generated.
  for (const auto& expr : block.getBody()) {
    if (!expr->isFunction()) continue;
    auto& funcAST = static_cast<FunctionAST&>(*expr);
    const PrototypeAST& proto = funcAST.getProto();

    // Skip lambdas, generics, externs, closures, and functions already declared
    if (proto.getName().empty()) continue;
    if (proto.isGeneric()) continue;
    if (funcAST.isExtern()) continue;
    if (proto.hasClosure()) continue;

    std::string funcName = proto.getQualifiedName();
    if (module->getFunction(funcName)) continue;

    // Build LLVM function type from resolved semantic types
    if (!proto.hasResolvedReturnType() || !proto.hasResolvedParamTypes())
      continue;

    llvm::Type* retType =
        typeResolver.resolveForReturn(proto.getResolvedReturnType());
    std::vector<llvm::Type*> paramTypes;
    for (const auto& sunType : proto.getResolvedParamTypes()) {
      paramTypes.push_back(typeResolver.resolve(sunType));
    }
    llvm::FunctionType* funcType =
        llvm::FunctionType::get(retType, paramTypes, false);
    llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, funcName,
                           module);
  }

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
      Value* funcVal = codegenFunc(funcAST);

      // Restore!
      if (currentBlock) {
        ctx.builder->SetInsertPoint(currentBlock);
      }

      // For nested named functions with closures, store the env pointer
      // in local scope so calls can pass it as the first argument
      const auto& proto = funcAST.getProto();
      if (!scopes.empty() && !proto.getName().empty() && proto.hasClosure()) {
        // funcVal is an env pointer - store it in scope
        if (funcVal) {
          scopes.back().variables[proto.getName()] =
              llvm::cast<llvm::AllocaInst>(funcVal);
        }
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
