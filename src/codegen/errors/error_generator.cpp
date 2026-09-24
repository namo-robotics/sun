// error_generator.cpp — How the error code reaches the scope stack
//
// One forwarder that cannot be inline in the header, because it needs the
// full definition of CodegenVisitor and the header is included by it.

#include "codegen/errors/error_generator.h"

#include "codegen/codegen_visitor.h"
#include "semantic_analysis/type_registry.h"

/** Generates control flow for throwing and catching Sun errors. */
namespace sun::codegen::errors {

sun::codegen::scopes::ScopeManager& ErrorGenerator::scopes() {
  return gen_.scopeManager();
}

llvm::Value* ErrorGenerator::codegen(const sun::ast::ExprAST& expr) {
  return gen_.codegen(expr);
}

llvm::Value* ErrorGenerator::codegen(const sun::ast::BlockExprAST& block) {
  return gen_.codegen(block);
}

std::shared_ptr<sun::semantic_analysis::TypeRegistry>&
ErrorGenerator::typeRegistry() {
  return state_.typeRegistry;
}

void ErrorGenerator::debugDeclareLocal(llvm::AllocaInst* alloca,
                                       const std::string& name,
                                       const sun::types::TypePtr& type,
                                       const sun::support::Position& loc) {
  state_.debugInfo.declareLocal(*ctx.builder, alloca, name, type, loc);
}

}  // namespace sun::codegen::errors
