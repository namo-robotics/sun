#pragma once

namespace sun::codegen {
class CodegenVisitor;
}
namespace sun::codegen::scopes {
class ScopeManager;
}

#include "ast.h"
#include "codegen/codegen_state.h"

namespace sun::codegen::enums {
using sun::semantic_analysis::EnumType;

/** Emits enum definitions, variants, matches, and payload cleanup. */
class EnumGenerator {
 public:
  /** Uses the shared state and services of the enclosing code generator. */
  EnumGenerator(sun::codegen::CodegenState& state,
                sun::codegen::CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        typeResolver(state.typeResolver) {}

  EnumGenerator(const EnumGenerator&) = delete;
  EnumGenerator& operator=(const EnumGenerator&) = delete;

  /** Prepares enum layouts, including generic specializations. */
  llvm::Value* codegen(const sun::ast::EnumDefinitionAST& expr);

  /** Constructs a variant and transfers its arguments into payload storage. */
  llvm::Value* codegenVariantConstruction(
      const sun::ast::CallExprAST& expr, EnumType& enumType,
      const sun::semantic_analysis::EnumVariant& variant);

  /** Emits a payload-free variant or a unit variant of a payload enum. */
  llvm::Value* codegenVariantAccess(
      EnumType& enumType, const sun::semantic_analysis::EnumVariant& variant);

  /** Matches the enum tag and emits the selected arm's payload bindings. */
  llvm::Value* codegenMatch(const sun::ast::MatchExprAST& expr,
                            EnumType& enumType);

  /** Drops owned payloads and invalidates the enum's storage. */
  void emitDrop(EnumType& enumType, llvm::Value* storagePtr);

 private:
  llvm::Function* getOrCreateDropFunction(EnumType& enumType);
  sun::codegen::scopes::ScopeManager& scopes();

  sun::codegen::CodegenState& state_;
  sun::codegen::CodegenVisitor& gen_;
  sun::codegen::CodegenContext& ctx;
  sun::codegen::LLVMTypeResolver& typeResolver;
};

}  // namespace sun::codegen::enums
