// thread_utils.h — Thread support utilities for code generation
//
// Provides the pthread-based emitters and thread structure types for
// spawn/join semantics. Thread creation and joining go through libc
// (pthread_create / pthread_join); the futex primitive that Mutex builds on
// has no libc wrapper, so it goes through libc's syscall() with a per-target
// syscall number — the one piece of per-target data in this file.

#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include "codegen/codegen.h"
#include "semantic_analysis/types.h"

/**
 * Utility class for thread-related code generation.
 *
 * Provides:
 * - Futex emitters (via libc syscall(), used by Mutex and _futex_* intrinsics)
 * - Thread structure types (context, handle)
 * - Thread trampoline generation (the pthread_create entry point)
 *
 * This class does NOT depend on CodegenVisitor, allowing it to be
 * used independently for thread-related IR generation.
 */
class ThreadUtils {
  CodegenContext& ctx;
  llvm::Module* module;

  // Cache for thread trampoline functions (keyed by lambda signature)
  std::map<std::string, llvm::Function*> trampolineCache;

 public:
  ThreadUtils(CodegenContext& ctx, llvm::Module* module)
      : ctx(ctx), module(module) {}

  // -------------------------------------------------------------------
  // Futex emitters
  // -------------------------------------------------------------------

  /**
   * Low-level futex(2) via libc syscall(). The futex syscall number is
   * per-target data resolved from the module's triple.
   *
   * @param addr Address of the futex word (i32*).
   * @param op Futex operation: FUTEX_WAIT (0) or FUTEX_WAKE (1).
   * @param val For WAIT: expected value (blocks if *addr == val).
   *            For WAKE: number of waiters to wake (usually 1).
   * @return 0 on success, -1 on failure (libc convention).
   */
  llvm::Value* emitSyscallFutex(llvm::Value* addr, llvm::Value* op,
                                llvm::Value* val);

  /**
   * Blocks until the futex word changes from the expected value.
   * Note: May return spuriously; caller should re-check condition in a loop.
   */
  void emitSyscallFutexWait(llvm::Value* addr, llvm::Value* expected);

  /**
   * Wakes at most one thread waiting on a futex.
   */
  void emitSyscallFutexWake(llvm::Value* addr);

  // -------------------------------------------------------------------
  // Thread trampoline
  // -------------------------------------------------------------------

  /**
   * Returns (creating on first use per lambda signature) the pthread entry
   * point for a spawned lambda:
   *
   *   ptr __sun_thread_start(ptr context)
   *
   * The trampoline loads {func, env} from the context, rebuilds the lambda
   * fat pointer, reads the arguments spawn moved into the context, calls the
   * lambda, releases the argument block, stores the result through the
   * context's result slot, and returns null. Memoized by lambda signature and
   * argument layout, since two spawns of same-typed lambdas share one
   * trampoline.
   *
   * @param contextType The layout of std.thread.ThreadContext, declared in
   *        Sun so the trampoline and the standard library cannot drift apart.
   * @param argsType The argument block's layout, or null when the lambda
   *        takes no arguments.
   */
  llvm::Function* getOrCreateThreadTrampoline(
      llvm::FunctionType* lambdaFuncType, llvm::StructType* fatType,
      llvm::Type* resultLLVMType, llvm::StructType* contextType,
      llvm::StructType* argsType);
};
