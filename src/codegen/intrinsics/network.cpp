// src/codegen/intrinsics/network.cpp - Network socket intrinsic codegen
//
// This file contains codegen for network socket intrinsics:
// - __socket, __bind, __listen, __accept, __connect
// - __send, __recv, __shutdown
// - __setsockopt, __getsockopt
//
// All socket operations call libc (see include/codegen/intrinsics/libc.h),
// which keeps the emitted IR target-neutral.

#include <llvm/TargetParser/Triple.h>

#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics_generator.h"
#include "codegen/intrinsics/libc.h"
#include "support/error.h"

using namespace llvm;

// ===================================================================
// Socket helper functions (thin libc forwarders)
// ===================================================================

// __sun_socket: socket(domain, type, protocol) -> fd
static Function* getOrCreateSocketHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  return sun::libc::forwarder(module, "__sun_socket", sun::libc::socket(module),
                              {i32Ty, i32Ty, i32Ty}, i32Ty);
}

// __sun_bind: bind(fd, addr, addrlen) -> result
static Function* getOrCreateBindHelper(llvm::Module* module,
                                       LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_bind", sun::libc::bind(module),
                              {i32Ty, ptrTy, i32Ty}, i32Ty);
}

// __sun_listen: listen(fd, backlog) -> result
static Function* getOrCreateListenHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  return sun::libc::forwarder(module, "__sun_listen", sun::libc::listen(module),
                              {i32Ty, i32Ty}, i32Ty);
}

// __sun_accept: accept(fd, addr, addrlen) -> client_fd
static Function* getOrCreateAcceptHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_accept", sun::libc::accept(module),
                              {i32Ty, ptrTy, ptrTy}, i32Ty);
}

// __sun_connect: connect(fd, addr, addrlen) -> result
static Function* getOrCreateConnectHelper(llvm::Module* module,
                                          LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_connect",
                              sun::libc::connect(module), {i32Ty, ptrTy, i32Ty},
                              i32Ty);
}

// __sun_send: send(fd, buf, len, flags) -> bytes_sent
static Function* getOrCreateSendHelper(llvm::Module* module,
                                       LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_send", sun::libc::send(module),
                              {i32Ty, ptrTy, i64Ty, i32Ty}, i64Ty);
}

// __sun_recv: recv(fd, buf, len, flags) -> bytes_received
static Function* getOrCreateRecvHelper(llvm::Module* module,
                                       LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_recv", sun::libc::recv(module),
                              {i32Ty, ptrTy, i64Ty, i32Ty}, i64Ty);
}

// __sun_shutdown: shutdown(fd, how) -> result
static Function* getOrCreateShutdownHelper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  return sun::libc::forwarder(module, "__sun_shutdown",
                              sun::libc::shutdown(module), {i32Ty, i32Ty},
                              i32Ty);
}

// __sun_setsockopt: setsockopt(fd, level, optname, optval, optlen) -> result
static Function* getOrCreateSetSockOptHelper(llvm::Module* module,
                                             LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_setsockopt",
                              sun::libc::setsockopt(module),
                              {i32Ty, i32Ty, i32Ty, ptrTy, i32Ty}, i32Ty);
}

// __sun_getsockopt: getsockopt(fd, level, optname, optval, optlen) -> result
static Function* getOrCreateGetSockOptHelper(llvm::Module* module,
                                             LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_getsockopt",
                              sun::libc::getsockopt(module),
                              {i32Ty, i32Ty, i32Ty, ptrTy, ptrTy}, i32Ty);
}

// -------------------------------------------------------------------
// Network socket codegen methods
// -------------------------------------------------------------------

// __socket(domain: i32, type: i32, protocol: i32) -> i32
Value* IntrinsicsGenerator::codegenSocket(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__socket expects 3 arguments: (domain: i32, type: i32, protocol: "
        "i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* domain = codegen(*expr.getArgs()[0]);
  if (!domain) return nullptr;
  Value* type = codegen(*expr.getArgs()[1]);
  if (!type) return nullptr;
  Value* protocol = codegen(*expr.getArgs()[2]);
  if (!protocol) return nullptr;

  if (!domain->getType()->isIntegerTy(32)) {
    domain = ctx.builder->CreateSExtOrTrunc(domain, Type::getInt32Ty(llvmCtx));
  }
  if (!type->getType()->isIntegerTy(32)) {
    type = ctx.builder->CreateSExtOrTrunc(type, Type::getInt32Ty(llvmCtx));
  }
  if (!protocol->getType()->isIntegerTy(32)) {
    protocol =
        ctx.builder->CreateSExtOrTrunc(protocol, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateSocketHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {domain, type, protocol}, "socket_fd");
}

// __bind(fd: i32, addr: raw_ptr<u8>, addrlen: i32) -> i32
Value* IntrinsicsGenerator::codegenBind(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__bind expects 3 arguments: (fd: i32, addr: raw_ptr<u8>, addrlen: "
        "i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* addr = codegen(*expr.getArgs()[1]);
  if (!addr) return nullptr;
  Value* addrlen = codegen(*expr.getArgs()[2]);
  if (!addrlen) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!addrlen->getType()->isIntegerTy(32)) {
    addrlen =
        ctx.builder->CreateSExtOrTrunc(addrlen, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateBindHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, addr, addrlen}, "bind_result");
}

// __listen(fd: i32, backlog: i32) -> i32
Value* IntrinsicsGenerator::codegenListen(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError("__listen expects 2 arguments: (fd: i32, backlog: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* backlog = codegen(*expr.getArgs()[1]);
  if (!backlog) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!backlog->getType()->isIntegerTy(32)) {
    backlog =
        ctx.builder->CreateSExtOrTrunc(backlog, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateListenHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, backlog}, "listen_result");
}

// __accept(fd: i32, addr: raw_ptr<u8>, addrlen: raw_ptr<i32>) -> i32
Value* IntrinsicsGenerator::codegenAccept(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__accept expects 3 arguments: (fd: i32, addr: raw_ptr<u8>, addrlen: "
        "raw_ptr<i32>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* addr = codegen(*expr.getArgs()[1]);
  if (!addr) return nullptr;
  Value* addrlen = codegen(*expr.getArgs()[2]);
  if (!addrlen) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateAcceptHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, addr, addrlen}, "accept_fd");
}

// __connect(fd: i32, addr: raw_ptr<u8>, addrlen: i32) -> i32
Value* IntrinsicsGenerator::codegenConnect(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__connect expects 3 arguments: (fd: i32, addr: raw_ptr<u8>, addrlen: "
        "i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* addr = codegen(*expr.getArgs()[1]);
  if (!addr) return nullptr;
  Value* addrlen = codegen(*expr.getArgs()[2]);
  if (!addrlen) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!addrlen->getType()->isIntegerTy(32)) {
    addrlen =
        ctx.builder->CreateSExtOrTrunc(addrlen, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateConnectHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, addr, addrlen}, "connect_result");
}

// __send(fd: i32, buf: raw_ptr<u8>, len: i64, flags: i32) -> i64
Value* IntrinsicsGenerator::codegenSend(const CallExprAST& expr) {
  if (expr.getArgs().size() != 4) {
    logAndThrowError(
        "__send expects 4 arguments: (fd: i32, buf: raw_ptr<u8>, len: i64, "
        "flags: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* buf = codegen(*expr.getArgs()[1]);
  if (!buf) return nullptr;
  Value* len = codegen(*expr.getArgs()[2]);
  if (!len) return nullptr;
  Value* flags = codegen(*expr.getArgs()[3]);
  if (!flags) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, Type::getInt64Ty(llvmCtx));
  }
  if (!flags->getType()->isIntegerTy(32)) {
    flags = ctx.builder->CreateSExtOrTrunc(flags, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateSendHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, buf, len, flags}, "send_result");
}

// __recv(fd: i32, buf: raw_ptr<u8>, len: i64, flags: i32) -> i64
Value* IntrinsicsGenerator::codegenRecv(const CallExprAST& expr) {
  if (expr.getArgs().size() != 4) {
    logAndThrowError(
        "__recv expects 4 arguments: (fd: i32, buf: raw_ptr<u8>, len: i64, "
        "flags: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* buf = codegen(*expr.getArgs()[1]);
  if (!buf) return nullptr;
  Value* len = codegen(*expr.getArgs()[2]);
  if (!len) return nullptr;
  Value* flags = codegen(*expr.getArgs()[3]);
  if (!flags) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, Type::getInt64Ty(llvmCtx));
  }
  if (!flags->getType()->isIntegerTy(32)) {
    flags = ctx.builder->CreateSExtOrTrunc(flags, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateRecvHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, buf, len, flags}, "recv_result");
}

// __shutdown(fd: i32, how: i32) -> i32
Value* IntrinsicsGenerator::codegenShutdown(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError("__shutdown expects 2 arguments: (fd: i32, how: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* how = codegen(*expr.getArgs()[1]);
  if (!how) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!how->getType()->isIntegerTy(32)) {
    how = ctx.builder->CreateSExtOrTrunc(how, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateShutdownHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, how}, "shutdown_result");
}

// __setsockopt(fd: i32, level: i32, optname: i32, optval: raw_ptr<u8>, optlen:
// i32) -> i32
Value* IntrinsicsGenerator::codegenSetSockOpt(const CallExprAST& expr) {
  if (expr.getArgs().size() != 5) {
    logAndThrowError(
        "__setsockopt expects 5 arguments: (fd: i32, level: i32, optname: i32, "
        "optval: raw_ptr<u8>, optlen: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* level = codegen(*expr.getArgs()[1]);
  if (!level) return nullptr;
  Value* optname = codegen(*expr.getArgs()[2]);
  if (!optname) return nullptr;
  Value* optval = codegen(*expr.getArgs()[3]);
  if (!optval) return nullptr;
  Value* optlen = codegen(*expr.getArgs()[4]);
  if (!optlen) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!level->getType()->isIntegerTy(32)) {
    level = ctx.builder->CreateSExtOrTrunc(level, Type::getInt32Ty(llvmCtx));
  }
  if (!optname->getType()->isIntegerTy(32)) {
    optname =
        ctx.builder->CreateSExtOrTrunc(optname, Type::getInt32Ty(llvmCtx));
  }
  if (!optlen->getType()->isIntegerTy(32)) {
    optlen = ctx.builder->CreateSExtOrTrunc(optlen, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateSetSockOptHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, level, optname, optval, optlen},
                                 "setsockopt_result");
}

// __getsockopt(fd: i32, level: i32, optname: i32, optval: raw_ptr<u8>, optlen:
// raw_ptr<i32>) -> i32
Value* IntrinsicsGenerator::codegenGetSockOpt(const CallExprAST& expr) {
  if (expr.getArgs().size() != 5) {
    logAndThrowError(
        "__getsockopt expects 5 arguments: (fd: i32, level: i32, optname: i32, "
        "optval: raw_ptr<u8>, optlen: raw_ptr<i32>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* level = codegen(*expr.getArgs()[1]);
  if (!level) return nullptr;
  Value* optname = codegen(*expr.getArgs()[2]);
  if (!optname) return nullptr;
  Value* optval = codegen(*expr.getArgs()[3]);
  if (!optval) return nullptr;
  Value* optlen = codegen(*expr.getArgs()[4]);
  if (!optlen) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!level->getType()->isIntegerTy(32)) {
    level = ctx.builder->CreateSExtOrTrunc(level, Type::getInt32Ty(llvmCtx));
  }
  if (!optname->getType()->isIntegerTy(32)) {
    optname =
        ctx.builder->CreateSExtOrTrunc(optname, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateGetSockOptHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, level, optname, optval, optlen},
                                 "getsockopt_result");
}

// ===================================================================
// High-level IPv4 socket helpers (build sockaddr_in internally)
// ===================================================================

// Fill a 16-byte stack sockaddr_in: family/port/addr, rest zeroed. Port and
// addr sit at offsets 2 and 4 everywhere; the first two bytes differ per OS.
// Linux has a 16-bit sa_family_t at offset 0; Darwin splits them into a
// one-byte sin_len (the struct size) followed by a one-byte sin_family — a
// little-endian 16-bit store of AF_INET there would set sin_len=2 and
// sin_family=0 (AF_UNSPEC), so the bytes are stored individually. Port
// arrives in host order; sin_port is big-endian, hence the bswap16.
static Value* buildSockaddrIn(IRBuilder<>& builder, LLVMContext& llvmCtx,
                              Value* ip, Value* port, bool isDarwin) {
  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* i16Ty = Type::getInt16Ty(llvmCtx);
  auto* i32Ty = Type::getInt32Ty(llvmCtx);

  Value* sockaddr =
      builder.CreateAlloca(i8Ty, builder.getInt32(16), "sockaddr");
  builder.CreateMemSet(sockaddr, builder.getInt8(0), 16, MaybeAlign(4));

  if (isDarwin) {
    // sin_len = sizeof(sockaddr_in), then sin_family = AF_INET (2)
    builder.CreateStore(builder.getInt8(16), sockaddr);
    Value* familyPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(1));
    builder.CreateStore(builder.getInt8(2), familyPtr);
  } else {
    // sin_family = AF_INET (2), a 16-bit field at offset 0
    builder.CreateStore(builder.getInt16(2), sockaddr);
  }

  Value* port16 = builder.CreateTrunc(port, i16Ty);
  Value* portHi = builder.CreateLShr(port16, 8);
  Value* portLo = builder.CreateShl(port16, 8);
  Value* portNet = builder.CreateOr(portHi, portLo);  // bswap16
  Value* portPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(2));
  builder.CreateStore(portNet, portPtr);

  // sin_addr at offset 4 (already network byte order from the caller)
  Value* addrPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(4));
  builder.CreateStore(builder.CreateTrunc(ip, i32Ty), addrPtr);

  return sockaddr;
}

// Shared body for __sun_bind_ipv4 / __sun_connect_ipv4: build the sockaddr
// and forward to the given libc function.
static Function* getOrCreateSockaddrCallHelper(llvm::Module* module,
                                               LLVMContext& llvmCtx,
                                               const char* wrapperName,
                                               llvm::FunctionCallee callee) {
  Function* func = module->getFunction(wrapperName);
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  FunctionType* funcTy = FunctionType::get(i32Ty, {i32Ty, i32Ty, i32Ty}, false);
  func =
      Function::Create(funcTy, Function::InternalLinkage, wrapperName, module);

  BasicBlock* entry = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entry);

  auto args = func->arg_begin();
  Value* fd = args++;
  Value* ip = args++;
  Value* port = args++;

  Value* sockaddr =
      buildSockaddrIn(builder, llvmCtx, ip, port,
                      llvm::Triple(module->getTargetTriple()).isOSDarwin());
  Value* result = builder.CreateCall(
      callee, {fd, sockaddr, builder.getInt32(16)}, "result");
  builder.CreateRet(result);
  return func;
}

// __sun_bind_ipv4(fd: i32, ip: i32, port: i32) -> i32
static Function* getOrCreateBindIPv4Helper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  return getOrCreateSockaddrCallHelper(module, llvmCtx, "__sun_bind_ipv4",
                                       sun::libc::bind(module));
}

// __sun_connect_ipv4(fd: i32, ip: i32, port: i32) -> i32
static Function* getOrCreateConnectIPv4Helper(llvm::Module* module,
                                              LLVMContext& llvmCtx) {
  return getOrCreateSockaddrCallHelper(module, llvmCtx, "__sun_connect_ipv4",
                                       sun::libc::connect(module));
}

// __sun_accept_fd(fd: i32) -> i32 — accept with NULL addr/addrlen
static Function* getOrCreateAcceptFdHelper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_accept_fd");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcTy = FunctionType::get(i32Ty, {i32Ty}, false);
  func = Function::Create(funcTy, Function::InternalLinkage, "__sun_accept_fd",
                          module);

  BasicBlock* entry = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entry);

  Value* fd = func->arg_begin();
  Value* nullPtr = ConstantPointerNull::get(cast<PointerType>(ptrTy));
  Value* result = builder.CreateCall(sun::libc::accept(module),
                                     {fd, nullPtr, nullPtr}, "client_fd");
  builder.CreateRet(result);
  return func;
}

// __bind_ipv4(fd: i32, ip: i32, port: i32) -> i32
Value* IntrinsicsGenerator::codegenBindIPv4(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__bind_ipv4 expects 3 arguments: (fd: i32, ip: i32, port: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* ip = codegen(*expr.getArgs()[1]);
  if (!ip) return nullptr;
  Value* port = codegen(*expr.getArgs()[2]);
  if (!port) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!ip->getType()->isIntegerTy(32)) {
    ip = ctx.builder->CreateSExtOrTrunc(ip, Type::getInt32Ty(llvmCtx));
  }
  if (!port->getType()->isIntegerTy(32)) {
    port = ctx.builder->CreateSExtOrTrunc(port, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateBindIPv4Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, ip, port}, "bind_ipv4_result");
}

// __connect_ipv4(fd: i32, ip: i32, port: i32) -> i32
Value* IntrinsicsGenerator::codegenConnectIPv4(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__connect_ipv4 expects 3 arguments: (fd: i32, ip: i32, port: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* ip = codegen(*expr.getArgs()[1]);
  if (!ip) return nullptr;
  Value* port = codegen(*expr.getArgs()[2]);
  if (!port) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!ip->getType()->isIntegerTy(32)) {
    ip = ctx.builder->CreateSExtOrTrunc(ip, Type::getInt32Ty(llvmCtx));
  }
  if (!port->getType()->isIntegerTy(32)) {
    port = ctx.builder->CreateSExtOrTrunc(port, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateConnectIPv4Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, ip, port}, "connect_ipv4_result");
}

// __accept_fd(fd: i32) -> i32
Value* IntrinsicsGenerator::codegenAcceptFd(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("__accept_fd expects 1 argument: (fd: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateAcceptFdHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd}, "accept_fd_result");
}

// Load sin_addr (offset 4, left in network order) and sin_port (offset 2,
// swapped to host order and widened to i32) out of a sockaddr_in, storing
// them through the two out-pointers.
static void extractSockaddrInParts(IRBuilder<>& builder, LLVMContext& llvmCtx,
                                   Value* sockaddr, Value* outIp,
                                   Value* outPort) {
  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* i16Ty = Type::getInt16Ty(llvmCtx);
  auto* i32Ty = Type::getInt32Ty(llvmCtx);

  Value* addrPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(4));
  Value* ip = builder.CreateLoad(i32Ty, addrPtr, "peer_ip");
  builder.CreateStore(ip, outIp);

  Value* portPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(2));
  Value* portNet = builder.CreateLoad(i16Ty, portPtr, "peer_port_net");
  Value* portHi = builder.CreateLShr(portNet, 8);
  Value* portLo = builder.CreateShl(portNet, 8);
  Value* port16 = builder.CreateOr(portHi, portLo);  // bswap16
  Value* port = builder.CreateZExt(port16, i32Ty, "peer_port");
  builder.CreateStore(port, outPort);
}

// __sun_sendto_ipv4(fd, buf, len, flags, ip, port) -> i64 — build the
// destination sockaddr and forward to sendto.
static Function* getOrCreateSendToIPv4Helper(llvm::Module* module,
                                             LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_sendto_ipv4");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcTy = FunctionType::get(
      i64Ty, {i32Ty, ptrTy, i64Ty, i32Ty, i32Ty, i32Ty}, false);
  func = Function::Create(funcTy, Function::InternalLinkage,
                          "__sun_sendto_ipv4", module);

  BasicBlock* entry = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entry);

  auto args = func->arg_begin();
  Value* fd = args++;
  Value* buf = args++;
  Value* len = args++;
  Value* flags = args++;
  Value* ip = args++;
  Value* port = args++;

  Value* sockaddr =
      buildSockaddrIn(builder, llvmCtx, ip, port,
                      llvm::Triple(module->getTargetTriple()).isOSDarwin());
  Value* result = builder.CreateCall(
      sun::libc::sendto(module),
      {fd, buf, len, flags, sockaddr, builder.getInt32(16)}, "result");
  builder.CreateRet(result);
  return func;
}

// __sun_recvfrom_ipv4(fd, buf, len, flags, out_ip, out_port) -> i64 — receive
// with a scratch sockaddr and report the sender through the out slots.
static Function* getOrCreateRecvFromIPv4Helper(llvm::Module* module,
                                               LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_recvfrom_ipv4");
  if (func) return func;

  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcTy = FunctionType::get(
      i64Ty, {i32Ty, ptrTy, i64Ty, i32Ty, ptrTy, ptrTy}, false);
  func = Function::Create(funcTy, Function::InternalLinkage,
                          "__sun_recvfrom_ipv4", module);

  BasicBlock* entry = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entry);

  auto args = func->arg_begin();
  Value* fd = args++;
  Value* buf = args++;
  Value* len = args++;
  Value* flags = args++;
  Value* outIp = args++;
  Value* outPort = args++;

  Value* sockaddr =
      builder.CreateAlloca(i8Ty, builder.getInt32(16), "sockaddr");
  builder.CreateMemSet(sockaddr, builder.getInt8(0), 16, MaybeAlign(4));
  Value* addrLen = builder.CreateAlloca(i32Ty, nullptr, "addrlen");
  builder.CreateStore(builder.getInt32(16), addrLen);

  Value* result =
      builder.CreateCall(sun::libc::recvfrom(module),
                         {fd, buf, len, flags, sockaddr, addrLen}, "result");
  // On failure the sockaddr is still the zeroed scratch, so the out slots get
  // harmless zeros; a wrapper throws before reading them.
  extractSockaddrInParts(builder, llvmCtx, sockaddr, outIp, outPort);
  builder.CreateRet(result);
  return func;
}

// __sun_getsockname_ipv4(fd, out_ip, out_port) -> i32 — report the socket's
// own bound address through the out slots.
static Function* getOrCreateGetSockNameIPv4Helper(llvm::Module* module,
                                                  LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_getsockname_ipv4");
  if (func) return func;

  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcTy = FunctionType::get(i32Ty, {i32Ty, ptrTy, ptrTy}, false);
  func = Function::Create(funcTy, Function::InternalLinkage,
                          "__sun_getsockname_ipv4", module);

  BasicBlock* entry = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entry);

  auto args = func->arg_begin();
  Value* fd = args++;
  Value* outIp = args++;
  Value* outPort = args++;

  Value* sockaddr =
      builder.CreateAlloca(i8Ty, builder.getInt32(16), "sockaddr");
  builder.CreateMemSet(sockaddr, builder.getInt8(0), 16, MaybeAlign(4));
  Value* addrLen = builder.CreateAlloca(i32Ty, nullptr, "addrlen");
  builder.CreateStore(builder.getInt32(16), addrLen);

  Value* result = builder.CreateCall(sun::libc::getsockname(module),
                                     {fd, sockaddr, addrLen}, "result");
  extractSockaddrInParts(builder, llvmCtx, sockaddr, outIp, outPort);
  builder.CreateRet(result);
  return func;
}

// __sendto_ipv4(fd: i32, buf: raw_ptr<u8>, len: i64, flags: i32, ip: i32,
// port: i32) -> i64
Value* IntrinsicsGenerator::codegenSendToIPv4(const CallExprAST& expr) {
  if (expr.getArgs().size() != 6) {
    logAndThrowError(
        "__sendto_ipv4 expects 6 arguments: (fd: i32, buf: raw_ptr<u8>, "
        "len: i64, flags: i32, ip: i32, port: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* buf = codegen(*expr.getArgs()[1]);
  if (!buf) return nullptr;
  Value* len = codegen(*expr.getArgs()[2]);
  if (!len) return nullptr;
  Value* flags = codegen(*expr.getArgs()[3]);
  if (!flags) return nullptr;
  Value* ip = codegen(*expr.getArgs()[4]);
  if (!ip) return nullptr;
  Value* port = codegen(*expr.getArgs()[5]);
  if (!port) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, i32Ty);
  }
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, i64Ty);
  }
  if (!flags->getType()->isIntegerTy(32)) {
    flags = ctx.builder->CreateSExtOrTrunc(flags, i32Ty);
  }
  if (!ip->getType()->isIntegerTy(32)) {
    ip = ctx.builder->CreateSExtOrTrunc(ip, i32Ty);
  }
  if (!port->getType()->isIntegerTy(32)) {
    port = ctx.builder->CreateSExtOrTrunc(port, i32Ty);
  }

  Function* helper = getOrCreateSendToIPv4Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, buf, len, flags, ip, port},
                                 "sendto_ipv4_result");
}

// __recvfrom_ipv4(fd: i32, buf: raw_ptr<u8>, len: i64, flags: i32,
// out_ip: raw_ptr<i32>, out_port: raw_ptr<i32>) -> i64
Value* IntrinsicsGenerator::codegenRecvFromIPv4(const CallExprAST& expr) {
  if (expr.getArgs().size() != 6) {
    logAndThrowError(
        "__recvfrom_ipv4 expects 6 arguments: (fd: i32, buf: raw_ptr<u8>, "
        "len: i64, flags: i32, out_ip: raw_ptr<i32>, out_port: raw_ptr<i32>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* buf = codegen(*expr.getArgs()[1]);
  if (!buf) return nullptr;
  Value* len = codegen(*expr.getArgs()[2]);
  if (!len) return nullptr;
  Value* flags = codegen(*expr.getArgs()[3]);
  if (!flags) return nullptr;
  Value* outIp = codegen(*expr.getArgs()[4]);
  if (!outIp) return nullptr;
  Value* outPort = codegen(*expr.getArgs()[5]);
  if (!outPort) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, i32Ty);
  }
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, i64Ty);
  }
  if (!flags->getType()->isIntegerTy(32)) {
    flags = ctx.builder->CreateSExtOrTrunc(flags, i32Ty);
  }

  Function* helper = getOrCreateRecvFromIPv4Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, buf, len, flags, outIp, outPort},
                                 "recvfrom_ipv4_result");
}

// __getsockname_ipv4(fd: i32, out_ip: raw_ptr<i32>, out_port: raw_ptr<i32>)
// -> i32
Value* IntrinsicsGenerator::codegenGetSockNameIPv4(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__getsockname_ipv4 expects 3 arguments: (fd: i32, "
        "out_ip: raw_ptr<i32>, out_port: raw_ptr<i32>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();

  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* outIp = codegen(*expr.getArgs()[1]);
  if (!outIp) return nullptr;
  Value* outPort = codegen(*expr.getArgs()[2]);
  if (!outPort) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateGetSockNameIPv4Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, outIp, outPort},
                                 "getsockname_ipv4_result");
}
