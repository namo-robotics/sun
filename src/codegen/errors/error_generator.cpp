// error_generator.cpp — How the error code reaches the scope stack
//
// One forwarder that cannot be inline in the header, because it needs the
// full definition of CodegenVisitor and the header is included by it.

#include "codegen/errors/error_generator.h"

#include "codegen/codegen_visitor.h"
#include "semantic_analysis/type_registry.h"

/** Generates concrete arithmetic errors, unsafe scopes, and call cleanup. */
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

}  // namespace sun::codegen::errors
