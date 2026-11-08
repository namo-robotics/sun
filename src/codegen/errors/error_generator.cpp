// error_generator.cpp — How the error code reaches the scope stack
//
// These forwarders need the full definition of CodegenVisitor, which includes
// this generator's header.

#include "codegen/errors/error_generator.h"

#include "codegen/codegen_visitor.h"

/** Generates unsafe scopes and call cleanup. */
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

}  // namespace sun::codegen::errors
