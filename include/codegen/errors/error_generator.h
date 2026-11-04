#pragma once

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {
class CodegenVisitor;
}
/** Provides the scope manager responsible for variable storage and cleanup. */
namespace sun::codegen::scopes {
class ScopeManager;
}

// Generates concrete arithmetic error methods and preserves owned values
// across scoped unsafe blocks and calls that may unwind.

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <type_traits>
#include <vector>

#include "ast.h"
#include "codegen/codegen_state.h"
#include "semantic_analysis/type_registry.h"

/** Generates value-based error support and call cleanup. */
namespace sun::codegen::errors {
using sun::ast::ExprAST;

/** Emits builtin error methods, unsafe blocks, and calls with cleanup. */
class ErrorGenerator {
 public:
  /** Binds error propagation generation to the shared expression visitor and
   * state. */
  ErrorGenerator(sun::codegen::CodegenState& state,
                 sun::codegen::CodegenVisitor& gen)
      : state_(state), gen_(gen), ctx(state.ctx), module(state.module) {}

  /** Binds error propagation generation to the shared expression visitor and
   * state. */
  ErrorGenerator(const ErrorGenerator&) = delete;
  /** Disallows assignment so ownership and object identity cannot be
   * duplicated. */
  ErrorGenerator& operator=(const ErrorGenerator&) = delete;

  /**
   * The body is emitted as written, in a scope of its own; the block's value
   * is handed out to the enclosing scope. Safety checks are done in sema.
   */
  llvm::Value* codegen(const sun::ast::UnsafeBlockAST& expr);

  /**
   * Applies the callee ABI and drops live owners if the call unwinds.
   */
  llvm::Value* emitPossiblyThrowingCall(llvm::FunctionType* fnTy,
                                        llvm::Value* callee,
                                        llvm::ArrayRef<llvm::Value*> args,
                                        bool canThrow, const llvm::Twine& name);
  /** Emits a declared callee using the same ABI and ownership cleanup rules. */
  llvm::Value* emitPossiblyThrowingCall(llvm::FunctionCallee callee,
                                        llvm::ArrayRef<llvm::Value*> args,
                                        bool canThrow, const llvm::Twine& name);

  /** Declares builtin error methods, optionally defining only referenced
   * methods. */
  void emitArithmeticErrorMethods(bool defineBodies = false);

 private:
  sun::codegen::CodegenState& state_;
  sun::codegen::CodegenVisitor& gen_;
  sun::codegen::CodegenContext& ctx;

  // Aliases into the shared state, so the emission code reads the same way
  // the rest of codegen does
  llvm::Module* module;

  /**
   * Delegates expression generation to the enclosing visitor.
   */
  llvm::Value* codegen(const ExprAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated
   * value. */
  llvm::Value* codegen(const sun::ast::BlockExprAST& block);

  /**
   * A node kind with its own overload must not silently bind to the
   * ExprAST forwarder above: that path attaches an expression debug location,
   * so a block routed through it changes DWARF output. Make it a compile
   * error instead. Add an overload here when a new kind is needed.
   */
  template <typename T>
    requires(!std::is_same_v<T, ExprAST> &&
             !std::is_same_v<T, sun::ast::BlockExprAST> &&
             std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  /** Provides the scope manager responsible for variable storage and cleanup.
   */
  sun::codegen::scopes::ScopeManager& scopes();
  /** Provides the semantic types available to this code generator. */
  std::shared_ptr<sun::semantic_analysis::TypeRegistry>& typeRegistry();
  /** Returns the personality function stored by this object. */
  llvm::Constant* getPersonalityFunction();

  /**
   * Ensure `fn` has a personality function set (needed for any function that
   * contains an invoke/landingpad). Idempotent.
   */
  void ensurePersonality(llvm::Function* fn);

};

}  // namespace sun::codegen::errors
