// intrinsics_generator.cpp — How the intrinsics reach the rest of codegen
//
// Two forwarders that cannot be inline in the header, because they need the
// full definition of CodegenVisitor and the header is included by it.

#include "codegen/intrinsics/intrinsics_generator.h"

#include "codegen/codegen_visitor.h"

/** Provides the generator for built-in operations. */
namespace sun::codegen::intrinsics {

llvm::Value* IntrinsicsGenerator::codegen(const sun::ast::ExprAST& expr) {
  return gen_.codegen(expr);
}

sun::codegen::scopes::ScopeManager& IntrinsicsGenerator::scopes() {
  return gen_.scopeManager();
}

}  // namespace sun::codegen::intrinsics
