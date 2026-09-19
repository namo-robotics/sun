// function_generator.cpp — How function codegen reaches the rest of codegen
//
// Forwarders that cannot be inline in the header, because they need the full
// definition of CodegenVisitor and the header is included by it.

#include "codegen/functions/function_generator.h"

#include "codegen/codegen_visitor.h"

/** Provides the registry of generated functions and their metadata. */
namespace sun::codegen::functions {

llvm::Value* FunctionGenerator::codegen(const sun::ast::ExprAST& expr) {
  return gen_.codegen(expr);
}

llvm::Value* FunctionGenerator::codegen(const sun::ast::BlockExprAST& block) {
  return gen_.codegen(block);
}

sun::codegen::scopes::ScopeManager& FunctionGenerator::scopes() {
  return gen_.scopeManager();
}

FunctionRegistry& FunctionGenerator::functions() {
  return gen_.functionRegistry();
}

sun::codegen::classes::ClassGenerator& FunctionGenerator::classes() {
  return gen_.classGenerator();
}

llvm::AllocaInst* FunctionGenerator::createEntryBlockAlloca(
    llvm::Function* func, llvm::StringRef name, llvm::Type* type) {
  return gen_.createEntryBlockAlloca(func, name, type);
}

void FunctionGenerator::debugDeclareParam(llvm::AllocaInst* alloca,
                                          const std::string& name,
                                          const sun::ast::PrototypeAST& proto,
                                          unsigned userArgIdx,
                                          unsigned argNoBase) {
  gen_.debugDeclareParam(alloca, name, proto, userArgIdx, argNoBase);
}

sun::codegen::abi::ExternCEmitter& FunctionGenerator::externC() {
  return gen_.externCEmitter();
}

llvm::Value* FunctionGenerator::applyMoveSemantics(
    llvm::Value* argVal, sun::types::TypePtr argSunType) {
  return gen_.applyMoveSemantics(argVal, std::move(argSunType));
}

}  // namespace sun::codegen::functions
