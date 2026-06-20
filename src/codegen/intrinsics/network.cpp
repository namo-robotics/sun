// src/codegen/intrinsics/network.cpp - Network socket intrinsic codegen
//
// This file contains codegen for network socket intrinsics:
// - __socket, __bind, __listen, __accept, __connect
// - __send, __recv, __shutdown
// - __setsockopt, __getsockopt
//
// All socket operations use raw Linux syscalls (no libc dependency).
//
// Linux x86_64 syscall numbers:
//   SYS_SOCKET     = 41
//   SYS_CONNECT    = 42
//   SYS_ACCEPT     = 43
//   SYS_SENDTO     = 44
//   SYS_RECVFROM   = 45
//   SYS_SHUTDOWN   = 48
//   SYS_BIND       = 49
//   SYS_LISTEN     = 50
//   SYS_SETSOCKOPT = 54
//   SYS_GETSOCKOPT = 55

#include "codegen_visitor.h"
#include "error.h"

using namespace llvm;

// Linux x86_64 syscall numbers for networking
static constexpr int64_t SYS_SOCKET = 41;
static constexpr int64_t SYS_CONNECT = 42;
static constexpr int64_t SYS_ACCEPT = 43;
static constexpr int64_t SYS_SENDTO = 44;
static constexpr int64_t SYS_RECVFROM = 45;
static constexpr int64_t SYS_SHUTDOWN = 48;
static constexpr int64_t SYS_BIND = 49;
static constexpr int64_t SYS_LISTEN = 50;
static constexpr int64_t SYS_SETSOCKOPT = 54;
static constexpr int64_t SYS_GETSOCKOPT = 55;

// ===================================================================
// Socket syscall helper functions
// ===================================================================

// __sun_socket: socket(domain, type, protocol) -> fd
static Function* getOrCreateSocketHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_socket");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // i32 __sun_socket(i32 domain, i32 type, i32 protocol)
  FunctionType* funcType =
      FunctionType::get(i32Ty, {i32Ty, i32Ty, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_socket",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* domain = &*argIt++;
  Value* type = &*argIt++;
  Value* protocol = &*argIt++;

  // socket(domain, type, protocol) - syscall 41
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_SOCKET);
  Value* domainExt = builder.CreateZExt(domain, i64Ty);
  Value* typeExt = builder.CreateZExt(type, i64Ty);
  Value* protocolExt = builder.CreateZExt(protocol, i64Ty);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, domainExt, typeExt, protocolExt}, "socket_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// __sun_bind: bind(fd, addr, addrlen) -> result
static Function* getOrCreateBindHelper(llvm::Module* module,
                                       LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_bind");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i32 __sun_bind(i32 fd, ptr addr, i32 addrlen)
  FunctionType* funcType =
      FunctionType::get(i32Ty, {i32Ty, ptrTy, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_bind",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* addr = &*argIt++;
  Value* addrlen = &*argIt++;

  // bind(fd, addr, addrlen) - syscall 49
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_BIND);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* addrlenExt = builder.CreateZExt(addrlen, i64Ty);

  Value* result = builder.CreateCall(syscallAsm,
                                     {sysno, fdExt, addr, addrlenExt},
                                     "bind_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// __sun_listen: listen(fd, backlog) -> result
static Function* getOrCreateListenHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_listen");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // i32 __sun_listen(i32 fd, i32 backlog)
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_listen",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* backlog = &*argIt++;

  // listen(fd, backlog) - syscall 50
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_LISTEN);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* backlogExt = builder.CreateZExt(backlog, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, fdExt, backlogExt, zero}, "listen_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// __sun_accept: accept(fd, addr, addrlen) -> new_fd
static Function* getOrCreateAcceptHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_accept");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i32 __sun_accept(i32 fd, ptr addr, ptr addrlen)
  // addr and addrlen can be null if caller doesn't need client address
  FunctionType* funcType =
      FunctionType::get(i32Ty, {i32Ty, ptrTy, ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_accept",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* addr = &*argIt++;
  Value* addrlen = &*argIt++;

  // accept(fd, addr, addrlen) - syscall 43
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, ptrTy, ptrTy};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_ACCEPT);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);

  Value* result =
      builder.CreateCall(syscallAsm, {sysno, fdExt, addr, addrlen},
                         "accept_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// __sun_connect: connect(fd, addr, addrlen) -> result
static Function* getOrCreateConnectHelper(llvm::Module* module,
                                          LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_connect");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i32 __sun_connect(i32 fd, ptr addr, i32 addrlen)
  FunctionType* funcType =
      FunctionType::get(i32Ty, {i32Ty, ptrTy, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_connect",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* addr = &*argIt++;
  Value* addrlen = &*argIt++;

  // connect(fd, addr, addrlen) - syscall 42
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_CONNECT);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* addrlenExt = builder.CreateZExt(addrlen, i64Ty);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, fdExt, addr, addrlenExt}, "connect_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// __sun_send: send(fd, buf, len, flags) -> bytes_sent
// Uses sendto with NULL addr/addrlen for connected sockets
static Function* getOrCreateSendHelper(llvm::Module* module,
                                       LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_send");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i64 __sun_send(i32 fd, ptr buf, i64 len, i32 flags)
  FunctionType* funcType =
      FunctionType::get(i64Ty, {i32Ty, ptrTy, i64Ty, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_send",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* buf = &*argIt++;
  Value* len = &*argIt++;
  Value* flags = &*argIt++;

  // sendto(fd, buf, len, flags, NULL, 0) - syscall 44
  // 6-arg syscall: rdi=fd, rsi=buf, rdx=len, r10=flags, r8=addr, r9=addrlen
  std::vector<Type*> paramTypes(7, i64Ty);
  paramTypes[2] = ptrTy;  // buf is pointer
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_SENDTO);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* flagsExt = builder.CreateZExt(flags, i64Ty);
  Value* nullAddr = ConstantInt::get(i64Ty, 0);  // NULL for connected socket
  Value* zeroLen = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, fdExt, buf, len, flagsExt, nullAddr, zeroLen},
      "send_result");
  builder.CreateRet(result);

  return func;
}

// __sun_recv: recv(fd, buf, len, flags) -> bytes_received
// Uses recvfrom with NULL addr/addrlen for connected sockets
static Function* getOrCreateRecvHelper(llvm::Module* module,
                                       LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_recv");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i64 __sun_recv(i32 fd, ptr buf, i64 len, i32 flags)
  FunctionType* funcType =
      FunctionType::get(i64Ty, {i32Ty, ptrTy, i64Ty, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_recv",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* buf = &*argIt++;
  Value* len = &*argIt++;
  Value* flags = &*argIt++;

  // recvfrom(fd, buf, len, flags, NULL, NULL) - syscall 45
  // 6-arg syscall: rdi=fd, rsi=buf, rdx=len, r10=flags, r8=addr, r9=addrlen
  std::vector<Type*> paramTypes(7, i64Ty);
  paramTypes[2] = ptrTy;  // buf is pointer
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_RECVFROM);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* flagsExt = builder.CreateZExt(flags, i64Ty);
  Value* nullAddr = ConstantInt::get(i64Ty, 0);  // NULL for connected socket
  Value* nullLen = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, fdExt, buf, len, flagsExt, nullAddr, nullLen},
      "recv_result");
  builder.CreateRet(result);

  return func;
}

// __sun_shutdown: shutdown(fd, how) -> result
static Function* getOrCreateShutdownHelper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_shutdown");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // i32 __sun_shutdown(i32 fd, i32 how)
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_shutdown", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* how = &*argIt++;

  // shutdown(fd, how) - syscall 48
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_SHUTDOWN);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* howExt = builder.CreateZExt(how, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, fdExt, howExt, zero},
                                     "shutdown_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// __sun_setsockopt: setsockopt(fd, level, optname, optval, optlen) -> result
static Function* getOrCreateSetSockOptHelper(llvm::Module* module,
                                             LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_setsockopt");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i32 __sun_setsockopt(i32 fd, i32 level, i32 optname, ptr optval, i32
  // optlen)
  FunctionType* funcType =
      FunctionType::get(i32Ty, {i32Ty, i32Ty, i32Ty, ptrTy, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_setsockopt", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* level = &*argIt++;
  Value* optname = &*argIt++;
  Value* optval = &*argIt++;
  Value* optlen = &*argIt++;

  // setsockopt(fd, level, optname, optval, optlen) - syscall 54
  // 5-arg syscall: rdi=fd, rsi=level, rdx=optname, r10=optval, r8=optlen
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_SETSOCKOPT);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* levelExt = builder.CreateZExt(level, i64Ty);
  Value* optnameExt = builder.CreateZExt(optname, i64Ty);
  Value* optlenExt = builder.CreateZExt(optlen, i64Ty);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, fdExt, levelExt, optnameExt, optval, optlenExt},
      "setsockopt_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// __sun_getsockopt: getsockopt(fd, level, optname, optval, optlen) -> result
static Function* getOrCreateGetSockOptHelper(llvm::Module* module,
                                             LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_getsockopt");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i32 __sun_getsockopt(i32 fd, i32 level, i32 optname, ptr optval, ptr
  // optlen)
  FunctionType* funcType =
      FunctionType::get(i32Ty, {i32Ty, i32Ty, i32Ty, ptrTy, ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_getsockopt", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* level = &*argIt++;
  Value* optname = &*argIt++;
  Value* optval = &*argIt++;
  Value* optlen = &*argIt++;

  // getsockopt(fd, level, optname, optval, optlen) - syscall 55
  // 5-arg syscall: rdi=fd, rsi=level, rdx=optname, r10=optval, r8=optlen
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty, ptrTy, ptrTy};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_GETSOCKOPT);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* levelExt = builder.CreateZExt(level, i64Ty);
  Value* optnameExt = builder.CreateZExt(optname, i64Ty);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, fdExt, levelExt, optnameExt, optval, optlen},
      "getsockopt_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// -------------------------------------------------------------------
// Network socket codegen methods
// -------------------------------------------------------------------

// __socket(domain: i32, type: i32, protocol: i32) -> i32
Value* CodegenVisitor::codegenSocket(const CallExprAST& expr) {
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
  return ctx.builder->CreateCall(helper, {domain, type, protocol},
                                 "socket_fd");
}

// __bind(fd: i32, addr: raw_ptr<u8>, addrlen: i32) -> i32
Value* CodegenVisitor::codegenBind(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenListen(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenAccept(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenConnect(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenSend(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenRecv(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenShutdown(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenSetSockOpt(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenGetSockOpt(const CallExprAST& expr) {
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

// Helper to create __sun_bind_ipv4 function that builds sockaddr_in and calls
// bind
static Function* getOrCreateBindIPv4Helper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_bind_ipv4");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* i16Ty = Type::getInt16Ty(llvmCtx);
  auto* ptrTy = PointerType::get(llvmCtx, 0);

  // __sun_bind_ipv4(fd: i32, ip: i32, port: i32) -> i32
  FunctionType* funcTy =
      FunctionType::get(i32Ty, {i32Ty, i32Ty, i32Ty}, false);
  func = Function::Create(funcTy, Function::InternalLinkage, "__sun_bind_ipv4",
                          module);

  BasicBlock* entry = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entry);

  auto args = func->arg_begin();
  Value* fd = args++;
  Value* ip = args++;
  Value* port = args++;

  // Allocate 16-byte sockaddr_in on stack
  Value* sockaddr = builder.CreateAlloca(i8Ty, builder.getInt32(16), "sockaddr");
  
  // Zero-initialize
  builder.CreateMemSet(sockaddr, builder.getInt8(0), 16, MaybeAlign(4));

  // Set sin_family = AF_INET (2) at offset 0 (2 bytes)
  Value* familyPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(0));
  builder.CreateStore(builder.getInt8(2), familyPtr);  // AF_INET = 2

  // Set sin_port at offset 2 (2 bytes, network byte order = big-endian)
  // port is in host order, need to swap to network order
  Value* port16 = builder.CreateTrunc(port, i16Ty);
  Value* portHi = builder.CreateLShr(port16, 8);
  Value* portLo = builder.CreateShl(port16, 8);
  Value* portNet = builder.CreateOr(portHi, portLo);  // bswap16
  Value* portPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(2));
  Value* portPtr16 = builder.CreateBitCast(portPtr, PointerType::get(i16Ty, 0));
  builder.CreateStore(portNet, portPtr16);

  // Set sin_addr at offset 4 (4 bytes, already in network byte order from
  // caller)
  Value* addrPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(4));
  Value* addrPtr32 = builder.CreateBitCast(addrPtr, PointerType::get(i32Ty, 0));
  builder.CreateStore(ip, addrPtr32);

  // Call bind syscall
  Value* sysnum = builder.getInt64(SYS_BIND);
  Value* fd64 = builder.CreateSExt(fd, i64Ty);
  Value* addrlen64 = builder.getInt64(16);

  FunctionType* syscallTy =
      FunctionType::get(i64Ty, {i64Ty, i64Ty, i64Ty, i64Ty}, false);
  InlineAsm* syscall = InlineAsm::get(
      syscallTy, "syscall", "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11}",
      true);
  Value* sockaddrInt = builder.CreatePtrToInt(sockaddr, i64Ty);
  Value* result =
      builder.CreateCall(syscall, {sysnum, fd64, sockaddrInt, addrlen64});
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// Helper to create __sun_connect_ipv4 function
static Function* getOrCreateConnectIPv4Helper(llvm::Module* module,
                                              LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_connect_ipv4");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* i16Ty = Type::getInt16Ty(llvmCtx);

  // __sun_connect_ipv4(fd: i32, ip: i32, port: i32) -> i32
  FunctionType* funcTy =
      FunctionType::get(i32Ty, {i32Ty, i32Ty, i32Ty}, false);
  func = Function::Create(funcTy, Function::InternalLinkage,
                          "__sun_connect_ipv4", module);

  BasicBlock* entry = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entry);

  auto args = func->arg_begin();
  Value* fd = args++;
  Value* ip = args++;
  Value* port = args++;

  // Allocate 16-byte sockaddr_in on stack
  Value* sockaddr = builder.CreateAlloca(i8Ty, builder.getInt32(16), "sockaddr");
  
  // Zero-initialize
  builder.CreateMemSet(sockaddr, builder.getInt8(0), 16, MaybeAlign(4));

  // Set sin_family = AF_INET (2)
  Value* familyPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(0));
  builder.CreateStore(builder.getInt8(2), familyPtr);

  // Set sin_port (network byte order)
  Value* port16 = builder.CreateTrunc(port, i16Ty);
  Value* portHi = builder.CreateLShr(port16, 8);
  Value* portLo = builder.CreateShl(port16, 8);
  Value* portNet = builder.CreateOr(portHi, portLo);
  Value* portPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(2));
  Value* portPtr16 = builder.CreateBitCast(portPtr, PointerType::get(i16Ty, 0));
  builder.CreateStore(portNet, portPtr16);

  // Set sin_addr
  Value* addrPtr = builder.CreateGEP(i8Ty, sockaddr, builder.getInt32(4));
  Value* addrPtr32 = builder.CreateBitCast(addrPtr, PointerType::get(i32Ty, 0));
  builder.CreateStore(ip, addrPtr32);

  // Call connect syscall
  Value* sysnum = builder.getInt64(SYS_CONNECT);
  Value* fd64 = builder.CreateSExt(fd, i64Ty);
  Value* addrlen64 = builder.getInt64(16);

  FunctionType* syscallTy =
      FunctionType::get(i64Ty, {i64Ty, i64Ty, i64Ty, i64Ty}, false);
  InlineAsm* syscall = InlineAsm::get(
      syscallTy, "syscall", "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11}",
      true);
  Value* sockaddrInt = builder.CreatePtrToInt(sockaddr, i64Ty);
  Value* result =
      builder.CreateCall(syscall, {sysnum, fd64, sockaddrInt, addrlen64});
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// Helper to create __sun_accept_fd function (simplified accept, ignores client
// addr)
static Function* getOrCreateAcceptFdHelper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_accept_fd");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // __sun_accept_fd(fd: i32) -> i32
  FunctionType* funcTy = FunctionType::get(i32Ty, {i32Ty}, false);
  func = Function::Create(funcTy, Function::InternalLinkage, "__sun_accept_fd",
                          module);

  BasicBlock* entry = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entry);

  Value* fd = func->arg_begin();

  // Call accept with NULL addr and addrlen
  Value* sysnum = builder.getInt64(SYS_ACCEPT);
  Value* fd64 = builder.CreateSExt(fd, i64Ty);
  Value* nullAddr = builder.getInt64(0);
  Value* nullLen = builder.getInt64(0);

  FunctionType* syscallTy =
      FunctionType::get(i64Ty, {i64Ty, i64Ty, i64Ty, i64Ty}, false);
  InlineAsm* syscall = InlineAsm::get(
      syscallTy, "syscall", "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11}",
      true);
  Value* result =
      builder.CreateCall(syscall, {sysnum, fd64, nullAddr, nullLen});
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// __bind_ipv4(fd: i32, ip: i32, port: i32) -> i32
Value* CodegenVisitor::codegenBindIPv4(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenConnectIPv4(const CallExprAST& expr) {
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
Value* CodegenVisitor::codegenAcceptFd(const CallExprAST& expr) {
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
