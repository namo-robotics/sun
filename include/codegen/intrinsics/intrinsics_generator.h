#pragma once

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {
class CodegenVisitor;
}
/** Provides the scope manager responsible for variable storage and cleanup. */
namespace sun::codegen::scopes {
class ScopeManager;
}

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

/** Provides the generator for built-in operations. */
namespace sun::codegen::intrinsics {
using sun::ast::CallExprAST;
using sun::ast::ExprAST;
using sun::semantic_analysis::TypePtr;

/**
 * Emits every intrinsic and built-in call. Holds the thread helpers it needs
 * for _spawn and _thread_join; everything else it reaches through the shared
 * state or the visitor it was given.
 */
class IntrinsicsGenerator {
 public:
  /** Binds built-in operation generation to the shared visitor and state. */
  IntrinsicsGenerator(sun::codegen::CodegenState& state,
                      sun::codegen::CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        module(state.module),
        typeResolver(state.typeResolver),
        threadUtils(state.ctx, state.module) {}

  /** Binds built-in operation generation to the shared visitor and state. */
  IntrinsicsGenerator(const IntrinsicsGenerator&) = delete;
  /** Disallows assignment so ownership and object identity cannot be duplicated. */
  IntrinsicsGenerator& operator=(const IntrinsicsGenerator&) = delete;

  /**
   * Generic intrinsics codegen (in intrinsics/generic.cpp)
   */
  llvm::Value* codegenSizeofIntrinsic(TypePtr typeArg);
  /** Emits LLVM code that constructs a value in supplied storage. */
  llvm::Value* codegenInitIntrinsic(
      TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args,
      const std::vector<sun::semantic_analysis::ArgConversion>& conversions,
      sun::semantic_analysis::DeclarationId constructor);
  /** Emits LLVM code that loads a typed value from an address. */
  llvm::Value* codegenLoadIntrinsic(
      TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  /** Emits LLVM code that stores a typed value at an address. */
  llvm::Value* codegenStoreIntrinsic(
      TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  /** Emits LLVM code that exposes an address as a raw pointer. */
  llvm::Value* codegenPtrAsRawIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  /** Emits LLVM code that obtains the address of a value. */
  llvm::Value* codegenAddressOfIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  /** Emits LLVM code that turns a pointer into a borrowed reference. */
  llvm::Value* codegenToRefIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  /** Emits LLVM code that tests whether a value matches a type. */
  llvm::Value* codegenIsIntrinsic(
      const TypePtr& target, const std::vector<std::unique_ptr<ExprAST>>& args);
  /** Emits LLVM code that destroys a value in supplied storage. */
  llvm::Value* codegenDeinitIntrinsic(
      TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  /** Emits the runtime call implementing the load i64 operation. */
  llvm::Value* codegenLoadI64Intrinsic(const CallExprAST& expr);
  /** Emits the runtime call implementing the store i64 operation. */
  llvm::Value* codegenStoreI64Intrinsic(const CallExprAST& expr);
  /** Emits LLVM code that allocates raw memory. */
  llvm::Value* codegenMallocIntrinsic(const CallExprAST& expr);
  /** Emits LLVM code that releases raw memory. */
  llvm::Value* codegenFreeIntrinsic(const CallExprAST& expr);
  /** Emits LLVM code that copies a range of memory bytes. */
  llvm::Value* codegenMemcpyIntrinsic(const CallExprAST& expr);
  /** Emit a byte copy that permits overlapping source and destination. */
  llvm::Value* codegenMemmoveIntrinsic(const CallExprAST& expr);
  /** Emits LLVM code that fills a range of memory bytes. */
  llvm::Value* codegenMemsetIntrinsic(const CallExprAST& expr);
  /** Decode an integer as an enum, returning None for unknown values. */
  llvm::Value* codegenEnumFromIntIntrinsic(
      const sun::ast::GenericCallAST& expr);
  /** Emits LLVM code that converts a value to the requested type. */
  llvm::Value* codegenConvertIntrinsic(
      TypePtr targetType, const std::vector<std::unique_ptr<ExprAST>>& args);
  /** Emits LLVM code that reinterprets a value with the requested representation. */
  llvm::Value* codegenBitcastIntrinsic(
      TypePtr targetType, const std::vector<std::unique_ptr<ExprAST>>& args);
  /** Emits LLVM code that advances a pointer by a byte offset. */
  llvm::Value* codegenPtrOffsetIntrinsic(const CallExprAST& expr);

  /**
   * Bit intrinsics (in intrinsics/bits.cpp)
   */
  llvm::Value* codegenMulHiU64Intrinsic(const CallExprAST& expr);
  /** Emits LLVM code that counts leading or trailing zero bits. */
  llvm::Value* codegenCountZerosIntrinsic(const CallExprAST& expr,
                                          bool leading);
  /**
   * Shared by _bswap_u16, _bswap_u32 and _bswap_u64
   */
  llvm::Value* codegenBswapIntrinsic(const CallExprAST& expr,
                                     unsigned bitWidth);

  /**
   * Atomic intrinsics (in atomic.cpp)
   */
  llvm::Value* codegenAtomicCmpxchgIntrinsic(const CallExprAST& expr,
                                             unsigned bitWidth,
                                             bool signedValues,
                                             const char* name);
  llvm::Value* codegenAtomicStoreIntrinsic(const CallExprAST& expr,
                                           unsigned bitWidth, bool signedValues,
                                           const char* name);
  llvm::Value* codegenAtomicLoadIntrinsic(const CallExprAST& expr,
                                          unsigned bitWidth, const char* name);
  llvm::Value* codegenAtomicFetchOpIntrinsic(const CallExprAST& expr,
                                             unsigned bitWidth,
                                             bool signedValues, bool subtract,
                                             const char* name);
  llvm::Value* codegenAtomicFenceIntrinsic(const CallExprAST& expr,
                                           bool acquire);

  /**
   * Futex intrinsics (in atomic.cpp)
   */
  llvm::Value* codegenFutexWaitIntrinsic(const CallExprAST& expr);
  /** Emits LLVM code that wakes threads waiting on a futex. */
  llvm::Value* codegenFutexWakeIntrinsic(const CallExprAST& expr);

  /**
   * Target intrinsics (in builtins.cpp)
   */
  llvm::Value* codegenTargetIsIntrinsic(const CallExprAST& expr);
  /**
   * Built-in intrinsics (libc calls; see intrinsics/libc.h). The registry
   * and dispatcher live in src/codegen/intrinsics/builtins.cpp; the codegen
   * methods below live in the per-area files beside it.
   */
  bool isBuiltinFunction(const std::string& name);
  /** Emits LLVM code that dispatches a recognized built-in operation. */
  llvm::Value* codegenBuiltin(const std::string& name, const CallExprAST& expr);

  /**
   * Print built-ins
   */
  llvm::Value* codegenPrintI32(const CallExprAST& expr);
  /** Emits the runtime call implementing the print i64 operation. */
  llvm::Value* codegenPrintI64(const CallExprAST& expr);
  /** Emits the runtime call implementing the print u64 operation. */
  llvm::Value* codegenPrintU64(const CallExprAST& expr);
  /** Emits the runtime call implementing the print f64 operation. */
  llvm::Value* codegenPrintF64(const CallExprAST& expr);
  /** Emits the runtime call implementing the print string operation. */
  llvm::Value* codegenPrintString(const CallExprAST& expr);
  /** Emits the runtime call implementing the print bytes operation. */
  llvm::Value* codegenPrintBytes(const CallExprAST& expr);
  /** Emits the runtime call implementing the print char operation. */
  llvm::Value* codegenPrintChar(const CallExprAST& expr);
  /** Emits the runtime call implementing the print newline operation. */
  llvm::Value* codegenPrintNewline();

  /**
   * File I/O built-ins
   */
  llvm::Value* codegenFileOpen(const CallExprAST& expr);
  /** Emits the runtime call implementing the file close operation. */
  llvm::Value* codegenFileClose(const CallExprAST& expr);
  /** Emits the runtime call implementing the file write operation. */
  llvm::Value* codegenFileWrite(const CallExprAST& expr);
  /** Emits the runtime call implementing the file read operation. */
  llvm::Value* codegenFileRead(const CallExprAST& expr);

  /**
   * Extended file I/O built-ins
   */
  llvm::Value* codegenLseek(const CallExprAST& expr);
  /** Emits the runtime call implementing the fstat operation. */
  llvm::Value* codegenFstat(const CallExprAST& expr);
  /** Emits the runtime call implementing the fsync operation. */
  llvm::Value* codegenFsync(const CallExprAST& expr);
  /** Emits the runtime call implementing the ftruncate operation. */
  llvm::Value* codegenFtruncate(const CallExprAST& expr);
  /** Emits the runtime call implementing the unlink operation. */
  llvm::Value* codegenUnlink(const CallExprAST& expr);
  /** Emits the runtime call implementing the rename operation. */
  llvm::Value* codegenRename(const CallExprAST& expr);
  /** Emits the runtime call implementing the mkdir operation. */
  llvm::Value* codegenMkdir(const CallExprAST& expr);
  /** Emits the runtime call implementing the rmdir operation. */
  llvm::Value* codegenRmdir(const CallExprAST& expr);
  /** Emits the runtime call implementing the write operation. */
  llvm::Value* codegenWrite(const CallExprAST& expr);
  /** Emits the runtime call implementing the read operation. */
  llvm::Value* codegenRead(const CallExprAST& expr);

  /**
   * Network socket built-ins
   */
  llvm::Value* codegenSocket(const CallExprAST& expr);
  /** Emits the runtime call implementing the bind operation. */
  llvm::Value* codegenBind(const CallExprAST& expr);
  /** Emits the runtime call implementing the listen operation. */
  llvm::Value* codegenListen(const CallExprAST& expr);
  /** Emits the runtime call implementing the accept operation. */
  llvm::Value* codegenAccept(const CallExprAST& expr);
  /** Emits the runtime call implementing the connect operation. */
  llvm::Value* codegenConnect(const CallExprAST& expr);
  /** Emits the runtime call implementing the send operation. */
  llvm::Value* codegenSend(const CallExprAST& expr);
  /** Emits the runtime call implementing the recv operation. */
  llvm::Value* codegenRecv(const CallExprAST& expr);
  /** Emits the runtime call implementing the shutdown operation. */
  llvm::Value* codegenShutdown(const CallExprAST& expr);
  /** Emits the runtime call implementing the set sock opt operation. */
  llvm::Value* codegenSetSockOpt(const CallExprAST& expr);
  /** Emits the runtime call implementing the get sock opt operation. */
  llvm::Value* codegenGetSockOpt(const CallExprAST& expr);

  /**
   * High-level IPv4 socket helpers (build sockaddr_in internally)
   */
  llvm::Value* codegenBindIPv4(const CallExprAST& expr);
  /** Emits the runtime call implementing the connect i pv4 operation. */
  llvm::Value* codegenConnectIPv4(const CallExprAST& expr);
  /** Emits the runtime call implementing the accept fd operation. */
  llvm::Value* codegenAcceptFd(const CallExprAST& expr);
  /** Emits the runtime call implementing the send to i pv4 operation. */
  llvm::Value* codegenSendToIPv4(const CallExprAST& expr);
  /** Emits the runtime call implementing the recv from i pv4 operation. */
  llvm::Value* codegenRecvFromIPv4(const CallExprAST& expr);
  /** Emits the runtime call implementing the get sock name i pv4 operation. */
  llvm::Value* codegenGetSockNameIPv4(const CallExprAST& expr);

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
      const TypePtr& lambdaSunType, const TypePtr& contextPtrType,
      const std::vector<std::unique_ptr<ExprAST>>& args,
      const std::vector<sun::semantic_analysis::ArgConversion>& conversions);

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
      const TypePtr& resultType,
      const std::vector<std::unique_ptr<ExprAST>>& args, bool dropResult);

  /**
   * The LLVM layout of std.thread.ThreadContext, read off the raw_ptr type
   * semantic analysis resolved rather than synthesized here, so there is one
   * definition of it and codegen never spells the class's name.
   */
  llvm::StructType* getThreadContextStruct(const TypePtr& contextPtrType);

 private:
  sun::codegen::CodegenState& state_;
  sun::codegen::CodegenVisitor& gen_;

  // Aliases into the shared state, so the emission code below reads the same
  // way the rest of codegen does
  sun::codegen::CodegenContext& ctx;
  llvm::Module* module;
  sun::codegen::LLVMTypeResolver& typeResolver;

  // Thread syscalls and types, used by _spawn and _thread_join
  ThreadUtils threadUtils;

  /**
   * Emit a nested expression by handing it back to the main dispatcher
   */
  llvm::Value* codegen(const ExprAST& expr);

  /**
   * The scope stack, for the intrinsics that drop a value in place
   */
  sun::codegen::scopes::ScopeManager& scopes();
};

}  // namespace sun::codegen::intrinsics
