#pragma once

// intrinsics_generator.h — Compiler intrinsics and libc built-ins
//
// Two families of call that never reach a user-written function body:
//
//   intrinsics  spelled with a leading underscore (_sizeof<T>, _malloc,
//               _atomic_load_i32, _spawn<F>) and lowered straight to IR
//   built-ins   thin wrappers over libc and syscalls (_print_i32, __socket,
//               __file_open) that the stdlib calls rather than declaring
//               extern "C" itself
//
// Both are pure emission: they read arguments, build instructions, and hand
// back a value. Nothing here owns compiler state beyond the thread helpers,
// which is why this is the piece of codegen that leans least on the rest.
// See intrinsics/intrinsics.h for what each name means.

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Value.h>

#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "codegen/codegen_state.h"
#include "codegen/intrinsics/thread_utils.h"
#include "semantic_analysis/argument_conversion.h"
#include "semantic_analysis/types.h"

class CodegenVisitor;
class ScopeManager;

/**
 * Emits every intrinsic and built-in call. Holds the thread helpers it needs
 * for _spawn and _thread_join; everything else it reaches through the shared
 * state or the visitor it was given.
 */
class IntrinsicsGenerator {
 public:
  IntrinsicsGenerator(CodegenState& state, CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        module(state.module),
        typeResolver(state.typeResolver),
        threadUtils(state.ctx, state.module) {}

  IntrinsicsGenerator(const IntrinsicsGenerator&) = delete;
  IntrinsicsGenerator& operator=(const IntrinsicsGenerator&) = delete;

  // Generic intrinsics codegen (in intrinsics/generic.cpp)
  llvm::Value* codegenSizeofIntrinsic(sun::TypePtr typeArg);
  llvm::Value* codegenInitIntrinsic(
      sun::TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenLoadIntrinsic(
      sun::TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenStoreIntrinsic(
      sun::TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenPtrAsRawIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenAddressOfIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenToRefIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenIsIntrinsic(
      const std::string& targetName,
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenDeinitIntrinsic(
      sun::TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenLoadI64Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenStoreI64Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenMallocIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenFreeIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenMemcpyIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenMemsetIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenConvertIntrinsic(
      sun::TypePtr targetType,
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenBitcastIntrinsic(
      sun::TypePtr targetType,
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenPtrOffsetIntrinsic(const CallExprAST& expr);

  // Bit intrinsics (in intrinsics/bits.cpp)
  llvm::Value* codegenMulHiU64Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenCountZerosIntrinsic(const CallExprAST& expr,
                                          bool leading);
  // Shared by _bswap_u16, _bswap_u32 and _bswap_u64
  llvm::Value* codegenBswapIntrinsic(const CallExprAST& expr,
                                     unsigned bitWidth);

  // Atomic intrinsics (in intrinsics.cpp)
  llvm::Value* codegenAtomicCmpxchgI32Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenAtomicStoreI32Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenAtomicLoadI32Intrinsic(const CallExprAST& expr);
  // Shared by _atomic_fetch_add_i32 and _atomic_fetch_sub_i32
  llvm::Value* codegenAtomicFetchOpI32Intrinsic(const CallExprAST& expr,
                                                bool subtract);

  // Futex intrinsics (in intrinsics.cpp)
  llvm::Value* codegenFutexWaitIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenFutexWakeIntrinsic(const CallExprAST& expr);

  // Target intrinsics (in builtins.cpp)
  llvm::Value* codegenTargetIsIntrinsic(const CallExprAST& expr);
  // Built-in intrinsics (libc calls; see intrinsics/libc.h). The registry
  // and dispatcher live in src/codegen/intrinsics/builtins.cpp; the codegen
  // methods below live in the per-area files beside it.
  bool isBuiltinFunction(const std::string& name);
  llvm::Value* codegenBuiltin(const std::string& name, const CallExprAST& expr);

  // Print built-ins
  llvm::Value* codegenPrintI32(const CallExprAST& expr);
  llvm::Value* codegenPrintI64(const CallExprAST& expr);
  llvm::Value* codegenPrintF64(const CallExprAST& expr);
  llvm::Value* codegenPrintString(const CallExprAST& expr);
  llvm::Value* codegenPrintBytes(const CallExprAST& expr);
  llvm::Value* codegenPrintChar(const CallExprAST& expr);
  llvm::Value* codegenPrintNewline();

  // File I/O built-ins
  llvm::Value* codegenFileOpen(const CallExprAST& expr);
  llvm::Value* codegenFileClose(const CallExprAST& expr);
  llvm::Value* codegenFileWrite(const CallExprAST& expr);
  llvm::Value* codegenFileRead(const CallExprAST& expr);

  // Extended file I/O built-ins
  llvm::Value* codegenLseek(const CallExprAST& expr);
  llvm::Value* codegenFstat(const CallExprAST& expr);
  llvm::Value* codegenFsync(const CallExprAST& expr);
  llvm::Value* codegenFtruncate(const CallExprAST& expr);
  llvm::Value* codegenUnlink(const CallExprAST& expr);
  llvm::Value* codegenRename(const CallExprAST& expr);
  llvm::Value* codegenMkdir(const CallExprAST& expr);
  llvm::Value* codegenRmdir(const CallExprAST& expr);
  llvm::Value* codegenWrite(const CallExprAST& expr);
  llvm::Value* codegenRead(const CallExprAST& expr);

  // Network socket built-ins
  llvm::Value* codegenSocket(const CallExprAST& expr);
  llvm::Value* codegenBind(const CallExprAST& expr);
  llvm::Value* codegenListen(const CallExprAST& expr);
  llvm::Value* codegenAccept(const CallExprAST& expr);
  llvm::Value* codegenConnect(const CallExprAST& expr);
  llvm::Value* codegenSend(const CallExprAST& expr);
  llvm::Value* codegenRecv(const CallExprAST& expr);
  llvm::Value* codegenShutdown(const CallExprAST& expr);
  llvm::Value* codegenSetSockOpt(const CallExprAST& expr);
  llvm::Value* codegenGetSockOpt(const CallExprAST& expr);

  // High-level IPv4 socket helpers (build sockaddr_in internally)
  llvm::Value* codegenBindIPv4(const CallExprAST& expr);
  llvm::Value* codegenConnectIPv4(const CallExprAST& expr);
  llvm::Value* codegenAcceptFd(const CallExprAST& expr);

  // -------------------------------------------------------------------
  // Thread support (uses ThreadUtils for syscalls and types)
  // -------------------------------------------------------------------

  /**
   * Generates IR for _spawn<F>(fn, args...).
   *
   * Builds the thread context on the heap — it must outlive this frame —
   * moves the arguments into an argument block beside it, and starts the
   * thread on a trampoline built for this lambda's signature. Hands back the
   * context pointer; stdlib `spawn` wraps that in the Thread<T> handle that
   * owns it, so the thread is joined when that handle is dropped.
   *
   * @param lambdaSunType The lambda type F was inferred as.
   * @param args The lambda followed by the arguments to move into the thread.
   * @param conversions One ArgConversion per entry of `args`.
   * @return The thread context pointer.
   */
  llvm::Value* codegenSpawnIntrinsic(
      const sun::TypePtr& lambdaSunType, const sun::TypePtr& contextPtrType,
      const std::vector<std::unique_ptr<ExprAST>>& args,
      const std::vector<sun::ArgConversion>& conversions);

  /**
   * Generates IR for _thread_join<T>(ctx) and _thread_join_drop<T>(ctx).
   *
   * Blocks until the thread has exited, then releases its context. Reading
   * the result out of the slot is a move: the caller takes over whatever it
   * owns. With `dropResult` nobody is taking it, so what the slot holds is
   * dropped in place first — freeing the slot alone would release the
   * result's own bytes and nothing they point at.
   *
   * @param resultType Sun type of the thread's result (T in Thread<T>).
   * @param args The thread context, as a single argument.
   * @param dropResult Drop the result rather than hand it back.
   * @return The thread's result, or a non-null placeholder for void.
   */
  llvm::Value* codegenThreadJoinIntrinsic(
      const sun::TypePtr& resultType,
      const std::vector<std::unique_ptr<ExprAST>>& args, bool dropResult);

  /**
   * The LLVM layout of std.thread.ThreadContext, read off the raw_ptr type
   * semantic analysis resolved rather than synthesized here, so there is one
   * definition of it and codegen never spells the class's name.
   */
  llvm::StructType* getThreadContextStruct(
      const sun::TypePtr& contextPtrType);

 private:
  CodegenState& state_;
  CodegenVisitor& gen_;

  // Aliases into the shared state, so the emission code below reads the same
  // way the rest of codegen does
  CodegenContext& ctx;
  llvm::Module* module;
  LLVMTypeResolver& typeResolver;

  // Thread syscalls and types, used by _spawn and _thread_join
  ThreadUtils threadUtils;

  // Emit a nested expression by handing it back to the main dispatcher
  llvm::Value* codegen(const ExprAST& expr);

  // The scope stack, for the intrinsics that drop a value in place
  ScopeManager& scopes();
};
