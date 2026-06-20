// src/codegen/intrinsics/io.cpp - File I/O intrinsic function codegen
//
// This file contains codegen for file I/O intrinsics:
// - __file_open, __file_close, __file_write, __file_read
// - __lseek, __fstat, __fsync, __ftruncate
// - __unlink, __rename, __mkdir, __rmdir
// - __write, __read (raw buffer operations)
//
// All I/O operations use raw Linux syscalls (no libc dependency).

#include "codegen_visitor.h"
#include "error.h"

using namespace llvm;

// ===================================================================
// File I/O built-in helpers (raw Linux x86_64 syscalls)
// ===================================================================

// Generic raw syscall emitter for 3-argument syscalls:
//   syscall(number, arg1, arg2, arg3) -> result
// Uses inline assembly on x86_64 Linux.
static Value* emitRawSyscall3(IRBuilder<>& builder, LLVMContext& llvmCtx,
                              Value* sysno, Value* arg1, Value* arg2,
                              Value* arg3) {
  // All args must be i64 for the inline asm constraint
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  sysno = builder.CreateZExtOrTrunc(sysno, i64Ty);
  arg1 = builder.CreateZExtOrTrunc(arg1, i64Ty);
  // arg2 can be a pointer, so we use ptrtoint if needed
  if (arg2->getType()->isPointerTy()) {
    arg2 = builder.CreatePtrToInt(arg2, i64Ty);
  } else {
    arg2 = builder.CreateZExtOrTrunc(arg2, i64Ty);
  }
  arg3 = builder.CreateZExtOrTrunc(arg3, i64Ty);

  std::vector<Type*> paramTypes(4, i64Ty);
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  InlineAsm* syscallAsm =
      InlineAsm::get(asmType, "syscall",
                     "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
                     /*hasSideEffects=*/true,
                     /*isAlignStack=*/false, InlineAsm::AD_ATT);

  return builder.CreateCall(syscallAsm, {sysno, arg1, arg2, arg3},
                            "syscall_result");
}

// Emit raw syscall with a pointer as arg2 (for read/write buf)
static Value* emitRawSyscall3Ptr(IRBuilder<>& builder, LLVMContext& llvmCtx,
                                 Value* sysno, Value* fd, Value* buf,
                                 Value* len) {
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // syscall write/read: rax=sysno, rdi=fd, rsi=buf(ptr), rdx=len
  std::vector<Type*> paramTypes = {i64Ty, i32Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  InlineAsm* syscallAsm =
      InlineAsm::get(asmType, "syscall",
                     "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
                     /*hasSideEffects=*/true,
                     /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysnoVal = builder.CreateZExtOrTrunc(sysno, i64Ty);
  Value* fdVal = builder.CreateZExtOrTrunc(fd, i32Ty);
  Value* lenVal = builder.CreateZExtOrTrunc(len, i64Ty);

  return builder.CreateCall(syscallAsm, {sysnoVal, fdVal, buf, lenVal},
                            "syscall_result");
}

// -------------------------------------------------------------------
// __sun_file_open: open(path, flags, mode) -> fd
// -------------------------------------------------------------------
static Function* getOrCreateFileOpenHelper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_file_open");
  if (func) return func;

  // i32 __sun_file_open(i8* path, i32 flags)
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_file_open", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* path = &*argIt++;
  Value* userFlags = &*argIt++;

  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // Map user flags to Linux open flags:
  //   0 -> O_RDONLY (0)
  //   1 -> O_WRONLY|O_CREAT|O_TRUNC (0x241)
  //   2 -> O_WRONLY|O_CREAT|O_APPEND (0x441)

  // Default: O_RDONLY
  BasicBlock* readBB = BasicBlock::Create(llvmCtx, "mode_read", func);
  BasicBlock* writeBB = BasicBlock::Create(llvmCtx, "mode_write", func);
  BasicBlock* appendBB = BasicBlock::Create(llvmCtx, "mode_append", func);
  BasicBlock* syscallBB = BasicBlock::Create(llvmCtx, "do_open", func);

  AllocaInst* flagsAlloca = builder.CreateAlloca(i32Ty, nullptr, "flags");
  builder.CreateStore(ConstantInt::get(i32Ty, 0), flagsAlloca);  // O_RDONLY

  Value* isWrite =
      builder.CreateICmpEQ(userFlags, ConstantInt::get(i32Ty, 1), "is_write");
  builder.CreateCondBr(isWrite, writeBB, readBB);

  // Read check -> could be append
  builder.SetInsertPoint(readBB);
  Value* isAppend =
      builder.CreateICmpEQ(userFlags, ConstantInt::get(i32Ty, 2), "is_append");
  builder.CreateCondBr(isAppend, appendBB, syscallBB);

  // Write mode: O_WRONLY|O_CREAT|O_TRUNC = 0x241 = 577
  builder.SetInsertPoint(writeBB);
  builder.CreateStore(ConstantInt::get(i32Ty, 577), flagsAlloca);
  builder.CreateBr(syscallBB);

  // Append mode: O_WRONLY|O_CREAT|O_APPEND = 0x441 = 1089
  builder.SetInsertPoint(appendBB);
  builder.CreateStore(ConstantInt::get(i32Ty, 1089), flagsAlloca);
  builder.CreateBr(syscallBB);

  // Do the syscall: sys_open = 2
  builder.SetInsertPoint(syscallBB);
  Value* openFlags = builder.CreateLoad(i32Ty, flagsAlloca);

  // mode = 0644 = 420 (for create)
  Value* mode = ConstantInt::get(i64Ty, 420);  // 0644 octal
  Value* sysno = ConstantInt::get(i64Ty, 2);   // sys_open

  // For open: rdi=path(ptr), so we need a version that takes ptr as arg1
  // open(const char *filename, int flags, umode_t mode)
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  InlineAsm* syscallAsm =
      InlineAsm::get(asmType, "syscall",
                     "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
                     /*hasSideEffects=*/true,
                     /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* flagsExt = builder.CreateZExt(openFlags, i64Ty);
  Value* result =
      builder.CreateCall(syscallAsm, {sysno, path, flagsExt, mode}, "fd");

  // Truncate result to i32 for the return
  Value* fd = builder.CreateTrunc(result, i32Ty, "fd32");
  builder.CreateRet(fd);

  return func;
}

// -------------------------------------------------------------------
// __sun_file_close: close(fd) -> result
// -------------------------------------------------------------------
static Function* getOrCreateFileCloseHelper(llvm::Module* module,
                                            LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_file_close");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_file_close", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  Value* fd = &*func->arg_begin();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // sys_close = 3, takes 1 arg: fd
  // We use a 3-arg syscall with dummy values for unused args
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 3);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, fdExt, zero, zero},
                                     "close_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// -------------------------------------------------------------------
// __sun_file_write: write(fd, str) -> bytes_written
// Writes a null-terminated string to the given fd.
// -------------------------------------------------------------------
static Function* getOrCreateFileWriteHelper(llvm::Module* module,
                                            LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_file_write");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i32 __sun_file_write(i32 fd, i8* str)
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty, ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_file_write", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  BasicBlock* loopBB = BasicBlock::Create(llvmCtx, "strlen_loop", func);
  BasicBlock* writeBB = BasicBlock::Create(llvmCtx, "write", func);

  IRBuilder<> builder(entryBB);
  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* strPtr = &*argIt++;

  // Calculate string length (manual strlen)
  AllocaInst* lenAlloca = builder.CreateAlloca(i64Ty, nullptr, "len");
  builder.CreateStore(ConstantInt::get(i64Ty, 0), lenAlloca);
  builder.CreateBr(loopBB);

  builder.SetInsertPoint(loopBB);
  Value* len = builder.CreateLoad(i64Ty, lenAlloca);
  Value* charPtr = builder.CreateGEP(i8Ty, strPtr, len);
  Value* ch = builder.CreateLoad(i8Ty, charPtr);
  Value* isNull = builder.CreateICmpEQ(ch, ConstantInt::get(i8Ty, 0));
  Value* newLen = builder.CreateAdd(len, ConstantInt::get(i64Ty, 1));
  builder.CreateStore(newLen, lenAlloca);
  builder.CreateCondBr(isNull, writeBB, loopBB);

  // Write to fd using sys_write (syscall 1)
  builder.SetInsertPoint(writeBB);
  Value* finalLen = builder.CreateLoad(i64Ty, lenAlloca);
  finalLen =
      builder.CreateSub(finalLen, ConstantInt::get(i64Ty, 1));  // exclude null

  Value* sysno = ConstantInt::get(i64Ty, 1);  // sys_write

  std::vector<Type*> paramTypes = {i64Ty, i32Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* result =
      builder.CreateCall(syscallAsm, {sysno, fd, strPtr, finalLen}, "written");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// -------------------------------------------------------------------
// __sun_file_read: read(fd, count) -> string
// Reads up to 'count' bytes from fd, returns heap-allocated string.
// Uses mmap for memory allocation (syscall 9).
// -------------------------------------------------------------------
static Function* getOrCreateFileReadHelper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_file_read");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i8* __sun_file_read(i32 fd, i32 count)
  FunctionType* funcType = FunctionType::get(ptrTy, {i32Ty, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_file_read", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  BasicBlock* readBB = BasicBlock::Create(llvmCtx, "do_read", func);
  BasicBlock* doneBB = BasicBlock::Create(llvmCtx, "done", func);

  IRBuilder<> builder(entryBB);
  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* count = &*argIt++;

  // Allocate buffer using mmap (syscall 9)
  // mmap(addr=0, length=count+1, prot=PROT_READ|PROT_WRITE=3,
  //      flags=MAP_PRIVATE|MAP_ANONYMOUS=0x22, fd=-1, offset=0)
  // We need a 6-arg syscall for mmap

  Value* countExt = builder.CreateZExt(count, i64Ty);
  Value* bufSize =
      builder.CreateAdd(countExt, ConstantInt::get(i64Ty, 1));  // +1 for null

  // 6-arg syscall for mmap
  // mmap uses: rax=sysno, rdi=addr, rsi=len, rdx=prot, r10=flags, r8=fd,
  // r9=offset We directly assign r10, r8, r9 as inputs instead of moving from
  // general regs
  std::vector<Type*> mmap6Types(7, i64Ty);  // sysno + 6 args
  FunctionType* mmap6FuncType = FunctionType::get(i64Ty, mmap6Types, false);
  InlineAsm* mmapAsm = InlineAsm::get(
      mmap6FuncType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* mmapSysno = ConstantInt::get(i64Ty, 9);  // sys_mmap
  Value* mmapAddr = ConstantInt::get(i64Ty, 0);   // NULL
  Value* mmapProt = ConstantInt::get(i64Ty, 3);   // PROT_READ|PROT_WRITE
  Value* mmapFlags =
      ConstantInt::get(i64Ty, 0x22);  // MAP_PRIVATE|MAP_ANONYMOUS
  Value* mmapFd = ConstantInt::getSigned(i64Ty, -1);
  Value* mmapOffset = ConstantInt::get(i64Ty, 0);

  Value* mmapResult = builder.CreateCall(
      mmapAsm,
      {mmapSysno, mmapAddr, bufSize, mmapProt, mmapFlags, mmapFd, mmapOffset},
      "mmap_result");

  Value* bufPtr = builder.CreateIntToPtr(mmapResult, ptrTy, "buf");
  builder.CreateBr(readBB);

  // sys_read(fd, buf, count) - syscall 0
  builder.SetInsertPoint(readBB);
  std::vector<Type*> readParamTypes = {i64Ty, i32Ty, ptrTy, i64Ty};
  FunctionType* readAsmType = FunctionType::get(i64Ty, readParamTypes, false);
  InlineAsm* readAsm = InlineAsm::get(
      readAsmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* readSysno = ConstantInt::get(i64Ty, 0);  // sys_read
  Value* readResult = builder.CreateCall(
      readAsm, {readSysno, fd, bufPtr, countExt}, "bytes_read");
  builder.CreateBr(doneBB);

  // Null-terminate the buffer at the position of bytes_read
  builder.SetInsertPoint(doneBB);

  // If read returned negative (error), clamp to 0
  Value* isNeg = builder.CreateICmpSLT(readResult, ConstantInt::get(i64Ty, 0));
  Value* safeLen =
      builder.CreateSelect(isNeg, ConstantInt::get(i64Ty, 0), readResult);

  Value* nullPtr = builder.CreateGEP(i8Ty, bufPtr, safeLen);
  builder.CreateStore(ConstantInt::get(i8Ty, 0), nullPtr);

  builder.CreateRet(bufPtr);
  return func;
}

// -------------------------------------------------------------------
// Extended file I/O helper functions
// -------------------------------------------------------------------

// __sun_lseek: lseek(fd, offset, whence) -> new_offset
static Function* getOrCreateLseekHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_lseek");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  FunctionType* funcType =
      FunctionType::get(i64Ty, {i32Ty, i64Ty, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_lseek",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* offset = &*argIt++;
  Value* whence = &*argIt++;

  // sys_lseek = 8
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 8);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* whenceExt = builder.CreateZExt(whence, i64Ty);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, fdExt, offset, whenceExt}, "lseek_result");
  builder.CreateRet(result);
  return func;
}

// __sun_fstat: fstat(fd, stat_buf) -> result
static Function* getOrCreateFstatHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_fstat");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty, ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_fstat",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* statBuf = &*argIt++;

  // sys_fstat = 5
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 5);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, fdExt, statBuf, zero},
                                     "fstat_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_fsync: fsync(fd) -> result
static Function* getOrCreateFsyncHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_fsync");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_fsync",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  Value* fd = &*func->arg_begin();

  // sys_fsync = 74
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 74);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, fdExt, zero, zero},
                                     "fsync_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_ftruncate: ftruncate(fd, length) -> result
static Function* getOrCreateFtruncateHelper(llvm::Module* module,
                                            LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_ftruncate");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty, i64Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_ftruncate", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* length = &*argIt++;

  // sys_ftruncate = 77
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 77);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, fdExt, length, zero},
                                     "ftruncate_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_unlink: unlink(path) -> result
static Function* getOrCreateUnlinkHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_unlink");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_unlink",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  Value* path = &*func->arg_begin();

  // sys_unlink = 87
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 87);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, path, zero, zero},
                                     "unlink_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_rename: rename(old_path, new_path) -> result
static Function* getOrCreateRenameHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_rename");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy, ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_rename",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* oldPath = &*argIt++;
  Value* newPath = &*argIt++;

  // sys_rename = 82
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 82);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, oldPath, newPath, zero}, "rename_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_mkdir: mkdir(path, mode) -> result
static Function* getOrCreateMkdirHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_mkdir");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_mkdir",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* path = &*argIt++;
  Value* mode = &*argIt++;

  // sys_mkdir = 83
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 83);
  Value* modeExt = builder.CreateZExt(mode, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, path, modeExt, zero},
                                     "mkdir_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_rmdir: rmdir(path) -> result
static Function* getOrCreateRmdirHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_rmdir");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_rmdir",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  Value* path = &*func->arg_begin();

  // sys_rmdir = 84
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 84);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result =
      builder.CreateCall(syscallAsm, {sysno, path, zero, zero}, "rmdir_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_write: write(fd, buf, len) -> bytes_written
static Function* getOrCreateWriteHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_write");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType =
      FunctionType::get(i64Ty, {i32Ty, ptrTy, i64Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_write",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* buf = &*argIt++;
  Value* len = &*argIt++;

  // sys_write = 1
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 1);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);

  Value* result =
      builder.CreateCall(syscallAsm, {sysno, fdExt, buf, len}, "write_result");
  builder.CreateRet(result);
  return func;
}

// __sun_read: read(fd, buf, len) -> bytes_read
static Function* getOrCreateReadHelper(llvm::Module* module,
                                       LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_read");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType =
      FunctionType::get(i64Ty, {i32Ty, ptrTy, i64Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_read",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* buf = &*argIt++;
  Value* len = &*argIt++;

  // sys_read = 0
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 0);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);

  Value* result =
      builder.CreateCall(syscallAsm, {sysno, fdExt, buf, len}, "read_result");
  builder.CreateRet(result);
  return func;
}

// -------------------------------------------------------------------
// File I/O codegen methods
// -------------------------------------------------------------------

// file_open(path: string, flags: i32) -> i32
Value* CodegenVisitor::codegenFileOpen(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "file_open expects 2 arguments: (path: string, flags: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* path = codegen(*expr.getArgs()[0]);
  if (!path) return nullptr;
  Value* flags = codegen(*expr.getArgs()[1]);
  if (!flags) return nullptr;

  // String literals are static_ptr<u8> which is a fat pointer struct { ptr, i64
  // } Extract the raw data pointer (element 0) for the syscall
  if (path->getType()->isStructTy()) {
    path = ctx.builder->CreateExtractValue(path, 0, "path.data");
  }

  // Ensure flags is i32
  if (!flags->getType()->isIntegerTy(32)) {
    flags = ctx.builder->CreateSExtOrTrunc(flags, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFileOpenHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {path, flags}, "fd");
}

// file_close(fd: i32) -> i32
Value* CodegenVisitor::codegenFileClose(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("file_close expects 1 argument: (fd: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFileCloseHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd}, "close_result");
}

// file_write(fd: i32, data: string) -> i32
Value* CodegenVisitor::codegenFileWrite(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError("file_write expects 2 arguments: (fd: i32, data: string)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* data = codegen(*expr.getArgs()[1]);
  if (!data) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  // String literals are static_ptr<u8> which is a fat pointer struct { ptr, i64
  // } Extract the raw data pointer (element 0) for the write syscall
  if (data->getType()->isStructTy()) {
    data = ctx.builder->CreateExtractValue(data, 0, "str.data");
  }

  Function* helper = getOrCreateFileWriteHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, data}, "written");
}

// file_read(fd: i32, count: i32) -> string
Value* CodegenVisitor::codegenFileRead(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError("file_read expects 2 arguments: (fd: i32, count: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* count = codegen(*expr.getArgs()[1]);
  if (!count) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!count->getType()->isIntegerTy(32)) {
    count = ctx.builder->CreateSExtOrTrunc(count, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFileReadHelper(module, llvmCtx);
  Value* result = ctx.builder->CreateCall(helper, {fd, count}, "read_str");

  // Track allocation metadata for automatic cleanup with munmap
  // Size is count+1 (for null terminator), same as what the helper allocates
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  Value* countExt = ctx.builder->CreateZExt(count, i64Ty);
  Value* mmapSize =
      ctx.builder->CreateAdd(countExt, ConstantInt::get(i64Ty, 1), "mmap.size");
  allocationMetadata[result] = {/*isMmap=*/true, /*size=*/mmapSize};

  return result;
}

// -------------------------------------------------------------------
// Extended file I/O codegen methods
// -------------------------------------------------------------------

// __lseek(fd: i32, offset: i64, whence: i32) -> i64
Value* CodegenVisitor::codegenLseek(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__lseek expects 3 arguments: (fd: i32, offset: i64, whence: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* offset = codegen(*expr.getArgs()[1]);
  if (!offset) return nullptr;
  Value* whence = codegen(*expr.getArgs()[2]);
  if (!whence) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!offset->getType()->isIntegerTy(64)) {
    offset = ctx.builder->CreateSExtOrTrunc(offset, Type::getInt64Ty(llvmCtx));
  }
  if (!whence->getType()->isIntegerTy(32)) {
    whence = ctx.builder->CreateSExtOrTrunc(whence, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateLseekHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, offset, whence}, "lseek_result");
}

// __fstat(fd: i32, stat_buf: raw_ptr<i8>) -> i32
Value* CodegenVisitor::codegenFstat(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "__fstat expects 2 arguments: (fd: i32, stat_buf: raw_ptr<i8>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* statBuf = codegen(*expr.getArgs()[1]);
  if (!statBuf) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFstatHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, statBuf}, "fstat_result");
}

// __fsync(fd: i32) -> i32
Value* CodegenVisitor::codegenFsync(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("__fsync expects 1 argument: (fd: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFsyncHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd}, "fsync_result");
}

// __ftruncate(fd: i32, length: i64) -> i32
Value* CodegenVisitor::codegenFtruncate(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError("__ftruncate expects 2 arguments: (fd: i32, length: i64)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* length = codegen(*expr.getArgs()[1]);
  if (!length) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!length->getType()->isIntegerTy(64)) {
    length = ctx.builder->CreateSExtOrTrunc(length, Type::getInt64Ty(llvmCtx));
  }

  Function* helper = getOrCreateFtruncateHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, length}, "ftruncate_result");
}

// __unlink(path: static_ptr<u8>) -> i32
Value* CodegenVisitor::codegenUnlink(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("__unlink expects 1 argument: (path: static_ptr<u8>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* path = codegen(*expr.getArgs()[0]);
  if (!path) return nullptr;

  // Extract raw pointer from static_ptr struct
  if (path->getType()->isStructTy()) {
    path = ctx.builder->CreateExtractValue(path, 0, "path.data");
  }

  Function* helper = getOrCreateUnlinkHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {path}, "unlink_result");
}

// __rename(old_path: static_ptr<u8>, new_path: static_ptr<u8>) -> i32
Value* CodegenVisitor::codegenRename(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "__rename expects 2 arguments: (old_path, new_path: static_ptr<u8>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* oldPath = codegen(*expr.getArgs()[0]);
  if (!oldPath) return nullptr;
  Value* newPath = codegen(*expr.getArgs()[1]);
  if (!newPath) return nullptr;

  // Extract raw pointers from static_ptr structs
  if (oldPath->getType()->isStructTy()) {
    oldPath = ctx.builder->CreateExtractValue(oldPath, 0, "old_path.data");
  }
  if (newPath->getType()->isStructTy()) {
    newPath = ctx.builder->CreateExtractValue(newPath, 0, "new_path.data");
  }

  Function* helper = getOrCreateRenameHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {oldPath, newPath}, "rename_result");
}

// __mkdir(path: static_ptr<u8>, mode: i32) -> i32
Value* CodegenVisitor::codegenMkdir(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "__mkdir expects 2 arguments: (path: static_ptr<u8>, mode: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* path = codegen(*expr.getArgs()[0]);
  if (!path) return nullptr;
  Value* mode = codegen(*expr.getArgs()[1]);
  if (!mode) return nullptr;

  // Extract raw pointer from static_ptr struct
  if (path->getType()->isStructTy()) {
    path = ctx.builder->CreateExtractValue(path, 0, "path.data");
  }
  if (!mode->getType()->isIntegerTy(32)) {
    mode = ctx.builder->CreateSExtOrTrunc(mode, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateMkdirHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {path, mode}, "mkdir_result");
}

// __rmdir(path: static_ptr<u8>) -> i32
Value* CodegenVisitor::codegenRmdir(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("__rmdir expects 1 argument: (path: static_ptr<u8>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* path = codegen(*expr.getArgs()[0]);
  if (!path) return nullptr;

  // Extract raw pointer from static_ptr struct
  if (path->getType()->isStructTy()) {
    path = ctx.builder->CreateExtractValue(path, 0, "path.data");
  }

  Function* helper = getOrCreateRmdirHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {path}, "rmdir_result");
}

// __write(fd: i32, buf: raw_ptr<u8>, len: i64) -> i64
Value* CodegenVisitor::codegenWrite(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__write expects 3 arguments: (fd: i32, buf: raw_ptr<u8>, len: i64)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* buf = codegen(*expr.getArgs()[1]);
  if (!buf) return nullptr;
  Value* len = codegen(*expr.getArgs()[2]);
  if (!len) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, Type::getInt64Ty(llvmCtx));
  }

  Function* helper = getOrCreateWriteHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, buf, len}, "write_result");
}

// __read(fd: i32, buf: raw_ptr<u8>, len: i64) -> i64
Value* CodegenVisitor::codegenRead(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__read expects 3 arguments: (fd: i32, buf: raw_ptr<u8>, len: i64)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* buf = codegen(*expr.getArgs()[1]);
  if (!buf) return nullptr;
  Value* len = codegen(*expr.getArgs()[2]);
  if (!len) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, Type::getInt64Ty(llvmCtx));
  }

  Function* helper = getOrCreateReadHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, buf, len}, "read_result");
}
