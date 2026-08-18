// block_expressions.cpp - Block expression codegen

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

// Declare one function signature without a body. The definition later in the
// block fills it in (codegen(PrototypeAST) reuses a matching declaration).
void CodegenVisitor::forwardDeclareFunction(const PrototypeAST& proto) {
  if (proto.getName().empty()) return;
  if (proto.hasClosure()) return;

  std::string funcName = proto.getMangledName();
  if (module->getFunction(funcName)) return;

  // Build LLVM function type from resolved semantic types
  if (!proto.hasResolvedReturnType() || !proto.hasResolvedParamTypes()) return;

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

// Pre-pass over a block: declare everything it defines before any body is
// emitted, so a call may name something defined further down — mutual
// recursion between functions, a method calling one of a class below it, a
// generic helper declared after its caller.
void CodegenVisitor::declareBlockSignatures(const BlockExprAST& block) {
  for (const auto& expr : block.getBody()) {
    if (expr->getType() == ASTNodeType::CLASS_DEFINITION) {
      declareBlockClassMethods(static_cast<const ClassDefinitionAST&>(*expr));
      continue;
    }
    if (!expr->isFunction()) continue;
    auto& funcAST = static_cast<FunctionAST&>(*expr);
    const PrototypeAST& proto = funcAST.getProto();

    // A generic function has no signature of its own — it is emitted as one
    // function per specialization. Declare those, so a call site earlier in
    // the block (a generic class method above the helper it calls, one generic
    // function calling another) finds the symbol.
    if (proto.isGeneric()) {
      for (const auto& [mangledName, specializedAST] :
           funcAST.getSpecializations()) {
        if (specializedAST) forwardDeclareFunction(specializedAST->getProto());
      }
      continue;
    }

    // C externs declare under their raw C symbol, not a mangled name, so
    // they can be called before their declaration appears in the file.
    // `declare` forward declarations are skipped: the real definition later
    // in the block supplies the mangled symbol.
    if (funcAST.isCExtern()) {
      codegenExternFunc(funcAST);
      continue;
    }
    if (funcAST.isExtern()) continue;

    forwardDeclareFunction(proto);
  }
}

Value* CodegenVisitor::codegen(const BlockExprAST& block) {
  if (block.isEmpty()) return ConstantFP::get(ctx.getContext(), APFloat(0.0));

  declareBlockSignatures(block);

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
