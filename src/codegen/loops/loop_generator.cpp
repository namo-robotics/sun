// loop_generator.cpp — How the loop code reaches the scope stack
//
// One forwarder that cannot be inline in the header, because it needs the
// full definition of CodegenVisitor and the header is included by it.

#include "codegen/loops/loop_generator.h"

#include "codegen/codegen_visitor.h"

ScopeManager& LoopGenerator::scopes() { return gen_.scopeManager(); }

llvm::Value* LoopGenerator::codegen(const ExprAST& expr) {
  return gen_.codegen(expr);
}

llvm::Value* LoopGenerator::codegen(const BlockExprAST& block) {
  return gen_.codegen(block);
}

FunctionRegistry& LoopGenerator::functions() { return gen_.functionRegistry(); }

llvm::AllocaInst* LoopGenerator::createEntryBlockAlloca(llvm::Function* func,
                                                        llvm::StringRef varName,
                                                        llvm::Type* type) {
  return gen_.createEntryBlockAlloca(func, varName, type);
}

void LoopGenerator::debugDeclareLocal(llvm::AllocaInst* alloca,
                                      const std::string& name,
                                      const sun::TypePtr& type,
                                      const Position& loc) {
  state_.debugInfo.declareLocal(*ctx.builder, alloca, name, type, loc);
}

llvm::Value* LoopGenerator::materializeMethodClosure(llvm::Value* fnPtr,
                                                     llvm::Value* receiverPtr,
                                                     llvm::StringRef name) {
  return gen_.materializeMethodClosure(fnPtr, receiverPtr, name);
}
