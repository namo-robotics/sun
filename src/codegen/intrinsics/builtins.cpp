// src/codegen/intrinsics/builtins.cpp - Builtin intrinsic dispatch
//
// The single registry of intrinsic names (isBuiltinFunction) and the
// dispatcher (codegenBuiltin) routing each name to its codegen method in
// the sibling files here (print.cpp, io.cpp, network.cpp, memory.cpp,
// atomic.cpp, generic.cpp).

#include "ast.h"
#include "codegen/codegen_visitor.h"

using namespace llvm;

// -------------------------------------------------------------------
// Built-in functions
// -------------------------------------------------------------------

bool CodegenVisitor::isBuiltinFunction(const std::string& name) {
  return name == "_print_i32" || name == "_print_i64" || name == "_print_f64" ||
         name == "_print_newline" || name == "_println_str" ||
         name == "_print_bytes" || name == "__file_open" ||
         name == "__file_close" || name == "__file_write" ||
         name == "__file_read" || name == "__lseek" || name == "__fstat" ||
         name == "__fsync" || name == "__ftruncate" || name == "__unlink" ||
         name == "__rename" || name == "__mkdir" || name == "__rmdir" ||
         name == "__write" || name == "__read" ||
         // Network socket intrinsics
         name == "__socket" || name == "__bind" || name == "__listen" ||
         name == "__accept" || name == "__connect" || name == "__send" ||
         name == "__recv" || name == "__shutdown" || name == "__setsockopt" ||
         name == "__getsockopt" ||
         // High-level IPv4 socket intrinsics
         name == "__bind_ipv4" || name == "__connect_ipv4" ||
         name == "__accept_fd" ||
         // Pointer intrinsics
         name == "_load_i64" || name == "_store_i64" ||
         // Memory allocation intrinsics
         name == "_malloc" || name == "_free" || name == "_memcpy" ||
         name == "_memset" || name == "_ptr_offset" ||
         // Atomic intrinsics
         name == "_atomic_cmpxchg_i32" || name == "_atomic_store_i32" ||
         name == "_atomic_load_i32" ||
         // Futex intrinsics
         name == "_futex_wait" || name == "_futex_wake" ||
         // Bit intrinsics
         name == "_mul_hi_u64" || name == "_ctlz_u64" || name == "_cttz_u64";
}

Value* CodegenVisitor::codegenBuiltin(const std::string& name,
                                      const CallExprAST& expr) {
  if (name == "_print_i32") {
    return codegenPrintI32(expr);
  }
  if (name == "_print_i64") {
    return codegenPrintI64(expr);
  }
  if (name == "_print_f64") {
    return codegenPrintF64(expr);
  }
  if (name == "_print_newline") {
    return codegenPrintNewline();
  }
  if (name == "_println_str") {
    Value* result = codegenPrintString(expr);
    codegenPrintNewline();
    return result;
  }
  if (name == "_print_bytes") {
    return codegenPrintBytes(expr);
  }
  if (name == "__file_open") {
    return codegenFileOpen(expr);
  }
  if (name == "__file_close") {
    return codegenFileClose(expr);
  }
  if (name == "__file_write") {
    return codegenFileWrite(expr);
  }
  if (name == "__file_read") {
    return codegenFileRead(expr);
  }
  if (name == "__lseek") {
    return codegenLseek(expr);
  }
  if (name == "__fstat") {
    return codegenFstat(expr);
  }
  if (name == "__fsync") {
    return codegenFsync(expr);
  }
  if (name == "__ftruncate") {
    return codegenFtruncate(expr);
  }
  if (name == "__unlink") {
    return codegenUnlink(expr);
  }
  if (name == "__rename") {
    return codegenRename(expr);
  }
  if (name == "__mkdir") {
    return codegenMkdir(expr);
  }
  if (name == "__rmdir") {
    return codegenRmdir(expr);
  }
  if (name == "__write") {
    return codegenWrite(expr);
  }
  if (name == "__read") {
    return codegenRead(expr);
  }
  // Pointer intrinsics
  if (name == "_load_i64") {
    return codegenLoadI64Intrinsic(expr);
  }
  if (name == "_store_i64") {
    return codegenStoreI64Intrinsic(expr);
  }
  // Memory allocation intrinsics
  if (name == "_malloc") {
    return codegenMallocIntrinsic(expr);
  }
  if (name == "_free") {
    return codegenFreeIntrinsic(expr);
  }
  if (name == "_memcpy") {
    return codegenMemcpyIntrinsic(expr);
  }
  if (name == "_memset") {
    return codegenMemsetIntrinsic(expr);
  }
  if (name == "_ptr_offset") {
    return codegenPtrOffsetIntrinsic(expr);
  }
  // Atomic intrinsics
  if (name == "_atomic_cmpxchg_i32") {
    return codegenAtomicCmpxchgI32Intrinsic(expr);
  }
  if (name == "_atomic_store_i32") {
    return codegenAtomicStoreI32Intrinsic(expr);
  }
  if (name == "_atomic_load_i32") {
    return codegenAtomicLoadI32Intrinsic(expr);
  }
  // Bit intrinsics
  if (name == "_mul_hi_u64") {
    return codegenMulHiU64Intrinsic(expr);
  }
  if (name == "_ctlz_u64" || name == "_cttz_u64") {
    return codegenCountZerosIntrinsic(expr, name == "_ctlz_u64");
  }
  // Futex intrinsics
  if (name == "_futex_wait") {
    return codegenFutexWaitIntrinsic(expr);
  }
  if (name == "_futex_wake") {
    return codegenFutexWakeIntrinsic(expr);
  }
  // Network socket intrinsics
  if (name == "__socket") {
    return codegenSocket(expr);
  }
  if (name == "__bind") {
    return codegenBind(expr);
  }
  if (name == "__listen") {
    return codegenListen(expr);
  }
  if (name == "__accept") {
    return codegenAccept(expr);
  }
  if (name == "__connect") {
    return codegenConnect(expr);
  }
  if (name == "__send") {
    return codegenSend(expr);
  }
  if (name == "__recv") {
    return codegenRecv(expr);
  }
  if (name == "__shutdown") {
    return codegenShutdown(expr);
  }
  if (name == "__setsockopt") {
    return codegenSetSockOpt(expr);
  }
  if (name == "__getsockopt") {
    return codegenGetSockOpt(expr);
  }
  // High-level IPv4 socket intrinsics
  if (name == "__bind_ipv4") {
    return codegenBindIPv4(expr);
  }
  if (name == "__connect_ipv4") {
    return codegenConnectIPv4(expr);
  }
  if (name == "__accept_fd") {
    return codegenAcceptFd(expr);
  }
  return nullptr;
}
