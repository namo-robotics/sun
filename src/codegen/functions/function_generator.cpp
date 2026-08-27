// function_generator.cpp — How function codegen reaches the rest of codegen
//
// Forwarders that cannot be inline in the header, because they need the full
// definition of CodegenVisitor and the header is included by it.

#include "codegen/functions/function_generator.h"

#include "codegen/codegen_visitor.h"

llvm::Value* FunctionGenerator::codegen(const ExprAST& expr) {
  return gen_.codegen(expr);
}

llvm::Value* FunctionGenerator::codegen(const BlockExprAST& block) {
  return gen_.codegen(block);
}

ScopeManager& FunctionGenerator::scopes() { return gen_.scopeManager(); }

FunctionRegistry& FunctionGenerator::functions() {
  return gen_.functionRegistry();
}

ClassGenerator& FunctionGenerator::classes() { return gen_.classGenerator(); }

llvm::AllocaInst* FunctionGenerator::createEntryBlockAlloca(
    llvm::Function* func, llvm::StringRef name, llvm::Type* type) {
  return gen_.createEntryBlockAlloca(func, name, type);
}

void FunctionGenerator::debugDeclareParam(llvm::AllocaInst* alloca,
                                          const std::string& name,
                                          const PrototypeAST& proto,
                                          unsigned userArgIdx,
                                          unsigned argNoBase) {
  gen_.debugDeclareParam(alloca, name, proto, userArgIdx, argNoBase);
}

sun::cabi::ExternCEmitter& FunctionGenerator::externC() {
  return gen_.externCEmitter();
}

llvm::LoadInst* FunctionGenerator::createLoadForLocalVar(
    const std::string& name) {
  return gen_.variableGenerator().createLoadForLocalVar(name);
}

llvm::LoadInst* FunctionGenerator::createLoadForGlobalVar(
    const std::string& varName) {
  return gen_.variableGenerator().createLoadForGlobalVar(varName);
}

llvm::Value* FunctionGenerator::applyMoveSemantics(llvm::Value* argVal,
                                                   sun::TypePtr argSunType) {
  return gen_.applyMoveSemantics(argVal, std::move(argSunType));
}
