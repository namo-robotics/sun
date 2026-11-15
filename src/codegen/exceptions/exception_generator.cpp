/** Connects exception generation to the shared expression visitor and scope
 * manager. */
#include "codegen/exceptions/exception_generator.h"

#include "codegen/codegen_visitor.h"

/** Generates unsafe scopes and call cleanup. */
namespace sun::codegen::exceptions {

sun::codegen::scopes::ScopeManager& ExceptionGenerator::scopes() {
  return gen_.scopeManager();
}

llvm::Value* ExceptionGenerator::codegen(const sun::ast::ExprAST& expr) {
  return gen_.codegen(expr);
}

llvm::Value* ExceptionGenerator::codegen(const sun::ast::BlockExprAST& block) {
  return gen_.codegen(block);
}

}  // namespace sun::codegen::exceptions
