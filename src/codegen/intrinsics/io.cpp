// src/codegen/intrinsics/io.cpp - File I/O intrinsic codegen
//
// __file_open/close/write/read plus the extended __lseek/__fstat/__fsync/
// __ftruncate/__unlink/__rename/__mkdir/__rmdir/__write/__read intrinsics.
// All operations call libc (see include/intrinsics/libc.h).

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"
#include "intrinsics/libc.h"

using namespace llvm;

// ===================================================================
// File I/O built-in helpers (libc calls; see include/intrinsics/libc.h)
// ===================================================================

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

  // Map user flags to open(2) flags. The O_* values are Linux's asm-generic
  // set, shared by x86-64 and aarch64:
  //   0 -> O_RDONLY (0)
  //   1 -> O_WRONLY|O_CREAT|O_TRUNC  (0x241)
  //   2 -> O_WRONLY|O_CREAT|O_APPEND (0x441)
  Value* isWrite =
      builder.CreateICmpEQ(userFlags, ConstantInt::get(i32Ty, 1), "is_write");
  Value* isAppend =
      builder.CreateICmpEQ(userFlags, ConstantInt::get(i32Ty, 2), "is_append");
  Value* flags = builder.CreateSelect(
      isWrite, ConstantInt::get(i32Ty, 0x241),
      builder.CreateSelect(isAppend, ConstantInt::get(i32Ty, 0x441),
                           ConstantInt::get(i32Ty, 0)),
      "open_flags");

  // mode = 0644, passed as a vararg per open(2)'s prototype
  Value* mode = ConstantInt::get(i32Ty, 0644);
  Value* fd =
      builder.CreateCall(sun::libc::open(module), {path, flags, mode}, "fd");
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
  Value* result =
      builder.CreateCall(sun::libc::close(module), {fd}, "close_result");
  builder.CreateRet(result);
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

  builder.SetInsertPoint(writeBB);
  Value* finalLen = builder.CreateLoad(i64Ty, lenAlloca);
  finalLen =
      builder.CreateSub(finalLen, ConstantInt::get(i64Ty, 1));  // exclude null

  Value* result = builder.CreateCall(sun::libc::write(module),
                                     {fd, strPtr, finalLen}, "written");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// -------------------------------------------------------------------
// __sun_file_read: read(fd, count) -> string
// Reads up to 'count' bytes from fd, returns a malloc'd null-terminated
// buffer the caller owns.
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
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* count = &*argIt++;

  Value* countExt = builder.CreateZExt(count, i64Ty);
  Value* bufSize =
      builder.CreateAdd(countExt, ConstantInt::get(i64Ty, 1));  // +1 for null
  Value* bufPtr =
      builder.CreateCall(sun::libc::malloc(module), {bufSize}, "buf");

  Value* readResult = builder.CreateCall(sun::libc::read(module),
                                         {fd, bufPtr, countExt}, "bytes_read");

  // If read returned negative (error), clamp to 0 and null-terminate there
  Value* isNeg = builder.CreateICmpSLT(readResult, ConstantInt::get(i64Ty, 0));
  Value* safeLen =
      builder.CreateSelect(isNeg, ConstantInt::get(i64Ty, 0), readResult);
  Value* nullPtr = builder.CreateGEP(i8Ty, bufPtr, safeLen);
  builder.CreateStore(ConstantInt::get(i8Ty, 0), nullPtr);

  builder.CreateRet(bufPtr);
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

  // The helper malloc()s the buffer; the caller owns it.
  Function* helper = getOrCreateFileReadHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, count}, "read_str");
}

// -------------------------------------------------------------------
// Extended file I/O helper functions
// -------------------------------------------------------------------

// __sun_lseek: lseek(fd, offset, whence) -> new_offset
static Function* getOrCreateLseekHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  return sun::libc::forwarder(module, "__sun_lseek",
                                  sun::libc::lseek(module),
                                  {i32Ty, i64Ty, i32Ty}, i64Ty);
}

// __sun_fstat: fstat(fd, stat_buf) -> result. The buffer layout is the
// target libc's struct stat; the Sun-side caller owns that interpretation.
static Function* getOrCreateFstatHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_fstat",
                                  sun::libc::fstat(module), {i32Ty, ptrTy},
                                  i32Ty);
}

// __sun_fsync: fsync(fd) -> result
static Function* getOrCreateFsyncHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  return sun::libc::forwarder(module, "__sun_fsync",
                                  sun::libc::fsync(module), {i32Ty}, i32Ty);
}

// __sun_ftruncate: ftruncate(fd, length) -> result
static Function* getOrCreateFtruncateHelper(llvm::Module* module,
                                            LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  return sun::libc::forwarder(module, "__sun_ftruncate",
                                  sun::libc::ftruncate(module),
                                  {i32Ty, i64Ty}, i32Ty);
}

// __sun_unlink: unlink(path) -> result
static Function* getOrCreateUnlinkHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_unlink",
                                  sun::libc::unlink(module), {ptrTy}, i32Ty);
}

// __sun_rename: rename(old_path, new_path) -> result
static Function* getOrCreateRenameHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_rename",
                                  sun::libc::rename(module), {ptrTy, ptrTy},
                                  i32Ty);
}

// __sun_mkdir: mkdir(path, mode) -> result
static Function* getOrCreateMkdirHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_mkdir",
                                  sun::libc::mkdir(module), {ptrTy, i32Ty},
                                  i32Ty);
}

// __sun_rmdir: rmdir(path) -> result
static Function* getOrCreateRmdirHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_rmdir",
                                  sun::libc::rmdir(module), {ptrTy}, i32Ty);
}

// __sun_write: write(fd, buf, len) -> bytes_written
static Function* getOrCreateWriteHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_write",
                                  sun::libc::write(module),
                                  {i32Ty, ptrTy, i64Ty}, i64Ty);
}

// __sun_read: read(fd, buf, len) -> bytes_read
static Function* getOrCreateReadHelper(llvm::Module* module,
                                       LLVMContext& llvmCtx) {
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  return sun::libc::forwarder(module, "__sun_read",
                                  sun::libc::read(module),
                                  {i32Ty, ptrTy, i64Ty}, i64Ty);
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
