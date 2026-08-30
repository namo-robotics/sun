#pragma once

// error_generator.h — throw, try/catch, and calls that may unwind
//
// Sun's error unions are native LLVM exceptions: a function declared
// `throws IError` returns a plain T and may unwind. So a call to one is an
// `invoke` whenever there is a landing pad to unwind to, and a plain call
// otherwise — which is the single decision `emitPossiblyThrowingCall` makes
// for every call site in codegen.
//
// Unwinding past a scope that still owns values has to drop them first. A try
// block therefore has two entry points for an exception: a plain landing pad
// when there is nothing to clean, and a per-call-site cleanup pad that drops
// the live owners and then joins the catch dispatch.

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <type_traits>
#include <vector>

#include "ast.h"
#include "codegen/codegen_state.h"

class CodegenVisitor;
class ScopeManager;

/**
 * One open try block. Throwing calls inside it are emitted as `invoke`s that
 * unwind to its landing pad, or — when scopes with live owners must be
 * cleaned first — to a per-call-site cleanup pad that drops them and then
 * branches into dispatchBB.
 */
struct TryContext {
  llvm::BasicBlock* landingPad;  // Plain landing pad (no owners to clean)
  llvm::BasicBlock* dispatchBB;  // Catch-clause dispatch (target of pads)
  llvm::PHINode* excPhi;         // Exception-pointer phi at dispatchBB entry
  size_t scopeDepth;             // Scope index of the try body scope
};

/**
 * Emits throw, try/catch, and every call that might unwind.
 */
class ErrorGenerator {
 public:
  ErrorGenerator(CodegenState& state, CodegenVisitor& gen)
      : state_(state), gen_(gen), ctx(state.ctx), module(state.module) {}

  ErrorGenerator(const ErrorGenerator&) = delete;
  ErrorGenerator& operator=(const ErrorGenerator&) = delete;

  llvm::Value* codegen(const TryCatchExprAST& expr);
  llvm::Value* codegen(const ThrowExprAST& expr);

  // The body is emitted as written, in a scope of its own; the block's value
  // is handed out to the enclosing scope. Safety checks are done in sema.
  llvm::Value* codegen(const UnsafeBlockAST& expr);

  /**
   * Emits a call that may unwind. If `canThrow` and we are inside a try
   * block, emits an `invoke` unwinding to the innermost try's landing pad and
   * continues codegen in the normal-destination block; otherwise emits a
   * plain call.
   */
  llvm::Value* emitPossiblyThrowingCall(llvm::FunctionType* fnTy,
                                        llvm::Value* callee,
                                        llvm::ArrayRef<llvm::Value*> args,
                                        bool canThrow, const llvm::Twine& name);
  llvm::Value* emitPossiblyThrowingCall(llvm::FunctionCallee callee,
                                        llvm::ArrayRef<llvm::Value*> args,
                                        bool canThrow, const llvm::Twine& name);

  // Integer division or modulo that throws instead of trapping when the
  // divisor is zero. Only used inside a function declared to return errors.
  llvm::Value* codegenSafeDivision(llvm::Value* L, llvm::Value* R,
                                   bool isModulo = false,
                                   bool isUnsigned = false);

 private:
  CodegenState& state_;
  CodegenVisitor& gen_;
  CodegenContext& ctx;

  // Stack of try contexts for error propagation to catch blocks
  std::vector<TryContext> tryStack;

  // Aliases into the shared state, so the emission code reads the same way
  // the rest of codegen does
  llvm::Module* module;

  // What throwing and catching borrow from the rest of codegen
  llvm::Value* codegen(const ExprAST& expr);
  llvm::Value* codegen(const BlockExprAST& block);

  // A node kind with its own overload must not silently bind to the
  // ExprAST forwarder above: that path attaches an expression debug location,
  // so a block routed through it changes DWARF output. Make it a compile
  // error instead. Add an overload here when a new kind is needed.
  template <typename T>
    requires(!std::is_same_v<T, ExprAST> && !std::is_same_v<T, BlockExprAST> &&
             std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  ScopeManager& scopes();
  std::shared_ptr<sun::TypeRegistry>& typeRegistry();
  void debugDeclareLocal(llvm::AllocaInst* alloca, const std::string& name,
                         const sun::TypePtr& type, const Position& loc);
  llvm::Value* createIntDivRem(llvm::Value* L, llvm::Value* R, bool isModulo,
                               bool isUnsigned);

  // True when the function being emitted may return errors
  bool currentFunctionCanError() const { return state_.frame.canError; }

  // Get or declare the C++ ABI exception handling functions
  llvm::FunctionCallee getCxaAllocateException();
  llvm::FunctionCallee getCxaThrow();
  llvm::FunctionCallee getCxaBeginCatch();
  llvm::FunctionCallee getCxaEndCatch();
  llvm::FunctionCallee getCxaRethrow();
  llvm::Constant* getPersonalityFunction();
  llvm::Constant* getSunExceptionTypeInfo();

  // Ensure `fn` has a personality function set (needed for any function that
  // contains an invoke/landingpad). Idempotent.
  void ensurePersonality(llvm::Function* fn);

  // Emit __cxa_throw(excPtr, tinfo, null) (as an invoke to the innermost
  // try's landing pad if inside a try, else a plain call), terminate the
  // current block with unreachable, and leave the builder in a fresh dead
  // block.
  void emitCxaThrowAndUnreachable(llvm::Value* excPtr);
};
