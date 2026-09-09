#pragma once

#include "ast.h"
#include "codegen/codegen_state.h"

class CodegenVisitor;
class ScopeManager;

/** Emits enum definitions, variants, matches, and payload cleanup. */
class EnumGenerator {
 public:
  /** Uses the shared state and services of the enclosing code generator. */
  EnumGenerator(CodegenState& state, CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        typeResolver(state.typeResolver) {}

  EnumGenerator(const EnumGenerator&) = delete;
  EnumGenerator& operator=(const EnumGenerator&) = delete;

  /** Prepares enum layouts, including generic specializations. */
  llvm::Value* codegen(const EnumDefinitionAST& expr);

  /** Constructs a variant and transfers its arguments into payload storage. */
  llvm::Value* codegenVariantConstruction(const CallExprAST& expr,
                                          sun::EnumType& enumType,
                                          const sun::EnumVariant& variant);

  /** Emits a payload-free variant or a unit variant of a payload enum. */
  llvm::Value* codegenVariantAccess(sun::EnumType& enumType,
                                    const sun::EnumVariant& variant);

  /** Matches the enum tag and emits the selected arm's payload bindings. */
  llvm::Value* codegenMatch(const MatchExprAST& expr, sun::EnumType& enumType);

  /** Drops owned payloads and invalidates the enum's storage. */
  void emitDrop(sun::EnumType& enumType, llvm::Value* storagePtr);

 private:
  llvm::Function* getOrCreateDropFunction(sun::EnumType& enumType);
  ScopeManager& scopes();

  CodegenState& state_;
  CodegenVisitor& gen_;
  CodegenContext& ctx;
  LLVMTypeResolver& typeResolver;
};
