// variable_generator.cpp — How variable codegen reaches the rest of codegen
//
// Forwarders that cannot be inline in the header, because they need the full
// definition of CodegenVisitor and the header is included by it.

#include "codegen/variables/variable_generator.h"

#include "codegen/codegen_visitor.h"

/** Generates storage and access operations for Sun variables. */
namespace sun::codegen::variables {

llvm::Value* VariableGenerator::codegen(const sun::ast::ExprAST& expr) {
  return gen_.codegen(expr);
}

llvm::Value* VariableGenerator::codegen(const sun::ast::BlockExprAST& block) {
  return gen_.codegen(block);
}

sun::codegen::scopes::ScopeManager& VariableGenerator::scopes() {
  return gen_.scopeManager();
}

sun::codegen::functions::FunctionRegistry& VariableGenerator::functions() {
  return gen_.functionRegistry();
}

sun::codegen::classes::ClassGenerator& VariableGenerator::classes() {
  return gen_.classGenerator();
}

sun::codegen::functions::FunctionGenerator& VariableGenerator::functionGen() {
  return gen_.functionGenerator();
}

llvm::AllocaInst* VariableGenerator::createEntryBlockAlloca(
    llvm::Function* func, llvm::StringRef name, llvm::Type* type) {
  return gen_.createEntryBlockAlloca(func, name, type);
}

void VariableGenerator::debugDeclareLocal(
    llvm::AllocaInst* alloca, const std::string& name,
    const sun::semantic_analysis::TypePtr& type,
    const sun::support::Position& loc) {
  state_.debugInfo.declareLocal(*ctx.builder, alloca, name, type, loc);
}

}  // namespace sun::codegen::variables
