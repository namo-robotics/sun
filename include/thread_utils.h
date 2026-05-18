// thread_utils.h — Thread support utilities for code generation
//
// Provides low-level syscall emitters and thread structure types
// for spawn/join semantics using raw Linux syscalls.

#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include "codegen.h"
#include "types.h"

/**
 * Utility class for thread-related code generation.
 *
 * Provides:
 * - Raw syscall emitters (mmap, munmap, clone, futex, exit)
 * - Thread structure types (context, handle)
 * - Thread trampoline generation
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
  // Raw syscall emitters
  // -------------------------------------------------------------------

  /**
   * Allocates anonymous memory via the mmap(2) syscall.
   *
   * Emits inline assembly for syscall 9 (mmap) with:
   *   - addr = NULL (kernel chooses address)
   *   - prot = PROT_READ | PROT_WRITE
   *   - flags = MAP_PRIVATE | MAP_ANONYMOUS
   *   - fd = -1, offset = 0
   *
   * @param size Number of bytes to allocate.
   * @return Pointer (i8*) to the allocated memory region.
   */
  llvm::Value* emitSyscallMmap(llvm::Value* size);

  /**
   * Releases memory previously allocated by emitSyscallMmap.
   *
   * Emits inline assembly for syscall 11 (munmap). The caller must
   * provide the exact size that was originally allocated.
   *
   * @param addr Pointer to the memory region to release.
   * @param size Size of the region in bytes (must match mmap allocation).
   */
  void emitSyscallMunmap(llvm::Value* addr, llvm::Value* size);

  /**
   * Creates a new thread via the clone(2) syscall.
   *
   * Emits inline assembly for syscall 56 (clone) with the provided flags.
   * The clone syscall returns twice: once in the parent (returns child TID)
   * and once in the child (returns 0). The caller must handle both cases
   * by branching on the return value.
   *
   * Typical flags for threads: CLONE_VM | CLONE_FS | CLONE_FILES |
   *   CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM |
   *   CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID
   *
   * @param flags Clone flags controlling shared resources and behavior.
   * @param stackTop Pointer to the TOP of the child's pre-allocated stack
   *                 (stacks grow downward on x86_64).
   * @param parentTidPtr Address where kernel writes child TID (for parent).
   * @param childTidPtr Address kernel clears on child exit (for futex wake).
   * @return Child's TID in parent process, 0 in child process.
   */
  llvm::Value* emitSyscallClone(llvm::Value* flags, llvm::Value* stackTop,
                                llvm::Value* parentTidPtr,
                                llvm::Value* childTidPtr);

  /**
   * Low-level futex(2) syscall for thread synchronization.
   *
   * Emits inline assembly for syscall 202 (futex). This is the primitive
   * used to implement join() - the parent waits on the futex word in the
   * thread context, and the child wakes it on exit.
   *
   * @param addr Address of the futex word (i32*).
   * @param op Futex operation: FUTEX_WAIT (0) or FUTEX_WAKE (1).
   * @param val For WAIT: expected value (blocks if *addr == val).
   *            For WAKE: number of waiters to wake (usually 1).
   * @return 0 on success, negative errno on failure.
   */
  llvm::Value* emitSyscallFutex(llvm::Value* addr, llvm::Value* op,
                                llvm::Value* val);

  /**
   * Blocks until the futex word changes from the expected value.
   *
   * Wrapper around emitSyscallFutex with FUTEX_WAIT. Used by join() to
   * wait for the child thread to complete. The child sets the futex word
   * to a different value and calls FUTEX_WAKE before exiting.
   *
   * Note: May return spuriously; caller should re-check condition in a loop.
   *
   * @param addr Address of the futex word (i32*).
   * @param expected Value that causes blocking (waits while *addr == expected).
   */
  void emitSyscallFutexWait(llvm::Value* addr, llvm::Value* expected);

  /**
   * Wakes threads waiting on a futex.
   *
   * Wrapper around emitSyscallFutex with FUTEX_WAKE. Called by the child
   * thread just before exit to unblock the parent's join() call.
   * Wakes at most one waiter.
   *
   * @param addr Address of the futex word (i32*).
   */
  void emitSyscallFutexWake(llvm::Value* addr);

  /**
   * Terminates the current thread via the exit(2) syscall.
   *
   * Emits inline assembly for syscall 60 (exit). Used by the child thread
   * after it has stored its result and signaled the futex. This does NOT
   * return - the thread ceases to exist.
   *
   * Note: For threads created with CLONE_THREAD, this exits only the
   * calling thread, not the entire process.
   *
   * @param exitCode Exit status (typically 0 for success).
   */
  void emitSyscallExit(llvm::Value* exitCode);

  // -------------------------------------------------------------------
  // Thread structure types
  // -------------------------------------------------------------------

  /**
   * Returns the LLVM struct type for thread context.
   *
   * Layout: {
   *   ptr func,         // Lambda function pointer
   *   ptr env,          // Lambda environment (captures)
   *   ptr result_slot,  // Where child stores return value
   *   i32 futex_word,   // Synchronization: 1=running, 0=done
   *   ptr stack_base,   // Base of mmap'd stack (for munmap)
   *   i64 stack_size    // Size of stack allocation
   * }
   *
   * Allocated by spawn(), passed to child thread, used by join() for cleanup.
   */
  llvm::StructType* getThreadContextType();

  /**
   * Returns the LLVM struct type for thread handle (returned by spawn).
   *
   * Layout: {
   *   ptr context,    // Pointer to ThreadContext struct
   *   ptr stack_base, // Stack base (for cleanup after join)
   *   i64 stack_size  // Stack size (for munmap after join)
   * }
   *
   * This is what spawn() returns and what join() consumes.
   */
  llvm::StructType* getThreadHandleType();

  // -------------------------------------------------------------------
  // Constants
  // -------------------------------------------------------------------

  /// Default thread stack size (2MB, matching Linux default)
  static constexpr int64_t DEFAULT_STACK_SIZE = 2 * 1024 * 1024;

  /// Clone flags for creating a thread (shares everything with parent)
  static constexpr int64_t THREAD_FLAGS =
      0x00000100 |  // CLONE_VM - Share memory space
      0x00000200 |  // CLONE_FS - Share filesystem info
      0x00000400 |  // CLONE_FILES - Share file descriptors
      0x00000800 |  // CLONE_SIGHAND - Share signal handlers
      0x00010000 |  // CLONE_THREAD - Same thread group
      0x00040000 |  // CLONE_SYSVSEM - Share SysV semaphores
      0x00100000 |  // CLONE_PARENT_SETTID - Set TID in parent
      0x00200000;   // CLONE_CHILD_CLEARTID - Clear TID in child on exit
};
