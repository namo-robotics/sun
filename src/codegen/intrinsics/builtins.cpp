// src/codegen/intrinsics/builtins.cpp - Builtin intrinsic dispatch
//
// One table maps every built-in name to the method that emits it. Recognising
// a name and lowering it are the same lookup, so the two can never drift
// apart. The methods themselves live in the sibling files here (print.cpp,
// io.cpp, network.cpp, memory.cpp, atomic.cpp, bits.cpp).

#include <functional>
#include <map>

#include "ast.h"
#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics_generator.h"
#include "support/error.h"
#include "support/target_os.h"

using namespace llvm;

namespace {

// What emitting one built-in takes: the generator to emit through, and the
// call being lowered.
using BuiltinEmitter =
    std::function<Value*(IntrinsicsGenerator&, const CallExprAST&)>;

// Most built-ins are a plain forward to one method; a few need an extra
// argument or a second call, so the table holds callables rather than
// member-function pointers.
const std::map<std::string, BuiltinEmitter>& builtinTable() {
  static const std::map<std::string, BuiltinEmitter> table = {
      // Print built-ins
      {"_print_i32",
       [](auto& g, const auto& e) { return g.codegenPrintI32(e); }},
      {"_print_i64",
       [](auto& g, const auto& e) { return g.codegenPrintI64(e); }},
      {"_print_f64",
       [](auto& g, const auto& e) { return g.codegenPrintF64(e); }},
      {"_print_newline",
       [](auto& g, const auto&) { return g.codegenPrintNewline(); }},
      {"_println_str",
       [](auto& g, const auto& e) {
         Value* result = g.codegenPrintString(e);
         g.codegenPrintNewline();
         return result;
       }},
      {"_print_char",
       [](auto& g, const auto& e) { return g.codegenPrintChar(e); }},
      {"_print_bytes",
       [](auto& g, const auto& e) { return g.codegenPrintBytes(e); }},

      // File I/O built-ins
      {"__file_open",
       [](auto& g, const auto& e) { return g.codegenFileOpen(e); }},
      {"__file_close",
       [](auto& g, const auto& e) { return g.codegenFileClose(e); }},
      {"__file_write",
       [](auto& g, const auto& e) { return g.codegenFileWrite(e); }},
      {"__file_read",
       [](auto& g, const auto& e) { return g.codegenFileRead(e); }},
      {"__lseek", [](auto& g, const auto& e) { return g.codegenLseek(e); }},
      {"__fstat", [](auto& g, const auto& e) { return g.codegenFstat(e); }},
      {"__fsync", [](auto& g, const auto& e) { return g.codegenFsync(e); }},
      {"__ftruncate",
       [](auto& g, const auto& e) { return g.codegenFtruncate(e); }},
      {"__unlink", [](auto& g, const auto& e) { return g.codegenUnlink(e); }},
      {"__rename", [](auto& g, const auto& e) { return g.codegenRename(e); }},
      {"__mkdir", [](auto& g, const auto& e) { return g.codegenMkdir(e); }},
      {"__rmdir", [](auto& g, const auto& e) { return g.codegenRmdir(e); }},
      {"__write", [](auto& g, const auto& e) { return g.codegenWrite(e); }},
      {"__read", [](auto& g, const auto& e) { return g.codegenRead(e); }},

      // Pointer intrinsics
      {"_load_i64",
       [](auto& g, const auto& e) { return g.codegenLoadI64Intrinsic(e); }},
      {"_store_i64",
       [](auto& g, const auto& e) { return g.codegenStoreI64Intrinsic(e); }},

      // Memory allocation intrinsics
      {"_malloc",
       [](auto& g, const auto& e) { return g.codegenMallocIntrinsic(e); }},
      {"_free",
       [](auto& g, const auto& e) { return g.codegenFreeIntrinsic(e); }},
      {"_memcpy",
       [](auto& g, const auto& e) { return g.codegenMemcpyIntrinsic(e); }},
      {"_memset",
       [](auto& g, const auto& e) { return g.codegenMemsetIntrinsic(e); }},
      {"_ptr_offset",
       [](auto& g, const auto& e) { return g.codegenPtrOffsetIntrinsic(e); }},

      // Atomic intrinsics
      {"_atomic_cmpxchg_i32",
       [](auto& g, const auto& e) {
         return g.codegenAtomicCmpxchgI32Intrinsic(e);
       }},
      {"_atomic_store_i32",
       [](auto& g, const auto& e) {
         return g.codegenAtomicStoreI32Intrinsic(e);
       }},
      {"_atomic_load_i32",
       [](auto& g, const auto& e) {
         return g.codegenAtomicLoadI32Intrinsic(e);
       }},
      {"_atomic_fetch_add_i32",
       [](auto& g, const auto& e) {
         return g.codegenAtomicFetchOpI32Intrinsic(e, /*subtract=*/false);
       }},
      {"_atomic_fetch_sub_i32",
       [](auto& g, const auto& e) {
         return g.codegenAtomicFetchOpI32Intrinsic(e, /*subtract=*/true);
       }},

      // Bit intrinsics
      {"_mul_hi_u64",
       [](auto& g, const auto& e) { return g.codegenMulHiU64Intrinsic(e); }},
      {"_ctlz_u64",
       [](auto& g, const auto& e) {
         return g.codegenCountZerosIntrinsic(e, /*leading=*/true);
       }},
      {"_cttz_u64",
       [](auto& g, const auto& e) {
         return g.codegenCountZerosIntrinsic(e, /*leading=*/false);
       }},
      {"_bswap_u16",
       [](auto& g, const auto& e) {
         return g.codegenBswapIntrinsic(e, /*bitWidth=*/16);
       }},
      {"_bswap_u32",
       [](auto& g, const auto& e) {
         return g.codegenBswapIntrinsic(e, /*bitWidth=*/32);
       }},
      {"_bswap_u64",
       [](auto& g, const auto& e) {
         return g.codegenBswapIntrinsic(e, /*bitWidth=*/64);
       }},

      // Futex intrinsics
      {"_futex_wait",
       [](auto& g, const auto& e) { return g.codegenFutexWaitIntrinsic(e); }},
      {"_futex_wake",
       [](auto& g, const auto& e) { return g.codegenFutexWakeIntrinsic(e); }},

      // Target intrinsics
      {"_target_is",
       [](auto& g, const auto& e) { return g.codegenTargetIsIntrinsic(e); }},

      // Network socket intrinsics
      {"__socket", [](auto& g, const auto& e) { return g.codegenSocket(e); }},
      {"__bind", [](auto& g, const auto& e) { return g.codegenBind(e); }},
      {"__listen", [](auto& g, const auto& e) { return g.codegenListen(e); }},
      {"__accept", [](auto& g, const auto& e) { return g.codegenAccept(e); }},
      {"__connect", [](auto& g, const auto& e) { return g.codegenConnect(e); }},
      {"__send", [](auto& g, const auto& e) { return g.codegenSend(e); }},
      {"__recv", [](auto& g, const auto& e) { return g.codegenRecv(e); }},
      {"__shutdown",
       [](auto& g, const auto& e) { return g.codegenShutdown(e); }},
      {"__setsockopt",
       [](auto& g, const auto& e) { return g.codegenSetSockOpt(e); }},
      {"__getsockopt",
       [](auto& g, const auto& e) { return g.codegenGetSockOpt(e); }},

      // High-level IPv4 socket intrinsics
      {"__bind_ipv4",
       [](auto& g, const auto& e) { return g.codegenBindIPv4(e); }},
      {"__connect_ipv4",
       [](auto& g, const auto& e) { return g.codegenConnectIPv4(e); }},
      {"__accept_fd",
       [](auto& g, const auto& e) { return g.codegenAcceptFd(e); }},
  };
  return table;
}

}  // namespace

bool IntrinsicsGenerator::isBuiltinFunction(const std::string& name) {
  return builtinTable().count(name) > 0;
}

Value* IntrinsicsGenerator::codegenBuiltin(const std::string& name,
                                           const CallExprAST& expr) {
  auto it = builtinTable().find(name);
  return it == builtinTable().end() ? nullptr : it->second(*this, expr);
}

Value* IntrinsicsGenerator::codegenTargetIsIntrinsic(const CallExprAST& expr) {
  // _target_is("macos") -> bool, folded to a constant for the compilation
  // target. The argument must be a string literal from the known set so a
  // typo is a compile error rather than a silently false branch. The
  // if/ternary codegen keeps only the live side of a branch on a constant,
  // which is what lets per-OS stdlib code declare externs (like Darwin's
  // __error) that other targets could not link.
  const auto& args = expr.getArgs();
  if (args.size() != 1 || args[0]->getType() != ASTNodeType::STRING_LITERAL) {
    logAndThrowError(
        "_target_is expects one string literal argument, e.g. "
        "_target_is(\"macos\")",
        expr.getLocation());
    return nullptr;
  }

  const std::string& name =
      static_cast<const StringLiteralAST&>(*args[0]).getValue();
  if (!sun::isKnownTargetOs(name)) {
    logAndThrowError("_target_is does not know the target '" + name +
                         "'; it accepts \"linux\", \"macos\" and \"windows\"",
                     expr.getLocation());
    return nullptr;
  }

  auto osName =
      sun::targetOsName(sun::resolvedTargetTriple(module->getTargetTriple()));
  bool result = osName && *osName == name;
  return ConstantInt::get(llvm::Type::getInt1Ty(ctx.getContext()),
                          result ? 1 : 0);
}
