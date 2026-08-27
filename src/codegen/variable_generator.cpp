// variable_generator.cpp — How variable codegen reaches the rest of codegen
//
// Forwarders that cannot be inline in the header, because they need the full
// definition of CodegenVisitor and the header is included by it.

#include "codegen/variable_generator.h"

#include "codegen/codegen_visitor.h"

llvm::Value* VariableGenerator::codegen(const ExprAST& expr) {
  return gen_.codegen(expr);
}

llvm::Value* VariableGenerator::codegen(const BlockExprAST& block) {
  return gen_.codegen(block);
}

ScopeManager& VariableGenerator::scopes() { return gen_.scopeManager(); }

FunctionRegistry& VariableGenerator::functions() {
  return gen_.functionRegistry();
}

ClassGenerator& VariableGenerator::classes() { return gen_.classGenerator(); }

FunctionGenerator& VariableGenerator::functionGen() {
  return gen_.functionGenerator();
}

llvm::AllocaInst* VariableGenerator::createEntryBlockAlloca(
    llvm::Function* func, llvm::StringRef name, llvm::Type* type) {
  return gen_.createEntryBlockAlloca(func, name, type);
}

void VariableGenerator::debugDeclareLocal(llvm::AllocaInst* alloca,
                                          const std::string& name,
                                          const sun::TypePtr& type,
                                          const Position& loc) {
  state_.debugInfo.declareLocal(*ctx.builder, alloca, name, type, loc);
}
