// error_generator.cpp — How the error code reaches the scope stack
//
// One forwarder that cannot be inline in the header, because it needs the
// full definition of CodegenVisitor and the header is included by it.

#include "codegen/errors/error_generator.h"

#include "codegen/codegen_visitor.h"

ScopeManager& ErrorGenerator::scopes() { return gen_.scopeManager(); }

llvm::Value* ErrorGenerator::codegen(const ExprAST& expr) {
  return gen_.codegen(expr);
}

llvm::Value* ErrorGenerator::codegen(const BlockExprAST& block) {
  return gen_.codegen(block);
}

std::shared_ptr<sun::TypeRegistry>& ErrorGenerator::typeRegistry() {
  return state_.typeRegistry;
}

void ErrorGenerator::debugDeclareLocal(llvm::AllocaInst* alloca,
                                       const std::string& name,
                                       const sun::TypePtr& type,
                                       const Position& loc) {
  state_.debugInfo.declareLocal(*ctx.builder, alloca, name, type, loc);
}

llvm::Value* ErrorGenerator::createIntDivRem(llvm::Value* L, llvm::Value* R,
                                             bool isModulo, bool isUnsigned) {
  return gen_.createIntDivRem(L, R, isModulo, isUnsigned);
}
