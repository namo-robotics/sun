// src/codegen/intrinsics/print.cpp - Print intrinsic codegen
//
// _print_i32/_print_i64/_print_f64/_print_newline/_println_str/_print_bytes.
// Integer formatting is emitted IR (portable); the final write goes through
// libc (see include/codegen/intrinsics/libc.h).

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/libc.h"

using namespace llvm;

static Value* emitLibcWriteInline(IRBuilder<>& builder, llvm::Module* module,
                                  Value* fd, Value* buf, Value* len) {
  LLVMContext& llvmCtx = module->getContext();
  Value* fdI32 =
      builder.CreateIntCast(fd, Type::getInt32Ty(llvmCtx), /*isSigned=*/true);
  return builder.CreateCall(sun::libc::write(module), {fdI32, buf, len},
                            "write_ret");
}

// -------------------------------------------------------------------
// Print helper functions
// -------------------------------------------------------------------

// Get or create the __sun_print_i32 helper function
static Function* getOrCreatePrintI32Helper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_print_i32");
  if (func) return func;

  // Create function: void __sun_print_i32(i32 %val)
  FunctionType* funcType = FunctionType::get(
      Type::getVoidTy(llvmCtx), {Type::getInt32Ty(llvmCtx)}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_print_i32", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  BasicBlock* loopBB = BasicBlock::Create(llvmCtx, "loop", func);
  BasicBlock* afterLoopBB = BasicBlock::Create(llvmCtx, "after_loop", func);
  BasicBlock* addMinusBB = BasicBlock::Create(llvmCtx, "add_minus", func);
  BasicBlock* writeBB = BasicBlock::Create(llvmCtx, "write", func);

  IRBuilder<> builder(entryBB);
  Value* val = func->arg_begin();

  // Buffer on stack
  llvm::Type* bufArrayType = ArrayType::get(Type::getInt8Ty(llvmCtx), 12);
  AllocaInst* buffer = builder.CreateAlloca(bufArrayType, nullptr, "buf");

  AllocaInst* idxAlloca = builder.CreateAlloca(Type::getInt32Ty(llvmCtx));
  builder.CreateStore(ConstantInt::get(Type::getInt32Ty(llvmCtx), 11),
                      idxAlloca);

  Value* isNegative = builder.CreateICmpSLT(
      val, ConstantInt::get(Type::getInt32Ty(llvmCtx), 0), "is_neg");
  Value* absVal = builder.CreateSelect(
      isNegative, builder.CreateNeg(val, "neg"), val, "abs");

  AllocaInst* numAlloca = builder.CreateAlloca(Type::getInt32Ty(llvmCtx));
  builder.CreateStore(absVal, numAlloca);
  builder.CreateBr(loopBB);

  // Loop: extract digits right to left
  builder.SetInsertPoint(loopBB);
  Value* num = builder.CreateLoad(Type::getInt32Ty(llvmCtx), numAlloca);
  Value* idx = builder.CreateLoad(Type::getInt32Ty(llvmCtx), idxAlloca);

  Value* digit =
      builder.CreateURem(num, ConstantInt::get(Type::getInt32Ty(llvmCtx), 10));
  Value* digitChar = builder.CreateAdd(
      digit, ConstantInt::get(Type::getInt32Ty(llvmCtx), '0'));
  Value* digitChar8 = builder.CreateTrunc(digitChar, Type::getInt8Ty(llvmCtx));

  Value* charPtr = builder.CreateGEP(Type::getInt8Ty(llvmCtx), buffer, idx);
  builder.CreateStore(digitChar8, charPtr);

  Value* newIdx =
      builder.CreateSub(idx, ConstantInt::get(Type::getInt32Ty(llvmCtx), 1));
  builder.CreateStore(newIdx, idxAlloca);
  Value* newNum =
      builder.CreateUDiv(num, ConstantInt::get(Type::getInt32Ty(llvmCtx), 10));
  builder.CreateStore(newNum, numAlloca);

  Value* cont = builder.CreateICmpUGT(
      newNum, ConstantInt::get(Type::getInt32Ty(llvmCtx), 0));
  builder.CreateCondBr(cont, loopBB, afterLoopBB);

  // After loop: check if negative
  builder.SetInsertPoint(afterLoopBB);
  builder.CreateCondBr(isNegative, addMinusBB, writeBB);

  // Add minus sign
  builder.SetInsertPoint(addMinusBB);
  Value* minusIdx = builder.CreateLoad(Type::getInt32Ty(llvmCtx), idxAlloca);
  Value* minusPtr =
      builder.CreateGEP(Type::getInt8Ty(llvmCtx), buffer, minusIdx);
  builder.CreateStore(ConstantInt::get(Type::getInt8Ty(llvmCtx), '-'),
                      minusPtr);
  builder.CreateStore(
      builder.CreateSub(minusIdx,
                        ConstantInt::get(Type::getInt32Ty(llvmCtx), 1)),
      idxAlloca);
  builder.CreateBr(writeBB);

  // Write to stdout
  builder.SetInsertPoint(writeBB);
  Value* finalIdx = builder.CreateLoad(Type::getInt32Ty(llvmCtx), idxAlloca);
  Value* startIdx = builder.CreateAdd(
      finalIdx, ConstantInt::get(Type::getInt32Ty(llvmCtx), 1));
  Value* startPtr =
      builder.CreateGEP(Type::getInt8Ty(llvmCtx), buffer, startIdx);
  Value* length = builder.CreateSub(
      ConstantInt::get(Type::getInt32Ty(llvmCtx), 12), startIdx);
  Value* length64 = builder.CreateZExt(length, Type::getInt64Ty(llvmCtx));
  Value* fd = ConstantInt::get(Type::getInt32Ty(llvmCtx), 1);

  emitLibcWriteInline(builder, module, fd, startPtr, length64);
  builder.CreateRetVoid();

  return func;
}

// Get or create the __sun_print_i64 helper function.
// Same digit-extraction shape as the i32 helper, widened to 64 bits.
// Buffer is 24 bytes: the longest output is "-9223372036854775808" (20 chars).
static Function* getOrCreatePrintI64Helper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_print_i64");
  if (func) return func;

  constexpr int kBufSize = 24;

  FunctionType* funcType = FunctionType::get(
      Type::getVoidTy(llvmCtx), {Type::getInt64Ty(llvmCtx)}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_print_i64", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  BasicBlock* loopBB = BasicBlock::Create(llvmCtx, "loop", func);
  BasicBlock* afterLoopBB = BasicBlock::Create(llvmCtx, "after_loop", func);
  BasicBlock* addMinusBB = BasicBlock::Create(llvmCtx, "add_minus", func);
  BasicBlock* writeBB = BasicBlock::Create(llvmCtx, "write", func);

  IRBuilder<> builder(entryBB);
  llvm::Type* i64Ty = Type::getInt64Ty(llvmCtx);
  llvm::Type* i32Ty = Type::getInt32Ty(llvmCtx);
  llvm::Type* i8Ty = Type::getInt8Ty(llvmCtx);
  Value* val = func->arg_begin();

  llvm::Type* bufArrayType = ArrayType::get(i8Ty, kBufSize);
  AllocaInst* buffer = builder.CreateAlloca(bufArrayType, nullptr, "buf");

  AllocaInst* idxAlloca = builder.CreateAlloca(i32Ty);
  builder.CreateStore(ConstantInt::get(i32Ty, kBufSize - 1), idxAlloca);

  Value* isNegative =
      builder.CreateICmpSLT(val, ConstantInt::get(i64Ty, 0), "is_neg");
  // Digits are extracted with unsigned div/rem, so negating INT64_MIN (which
  // overflows back to itself) still yields the correct magnitude bit pattern.
  Value* absVal = builder.CreateSelect(
      isNegative, builder.CreateNeg(val, "neg"), val, "abs");

  AllocaInst* numAlloca = builder.CreateAlloca(i64Ty);
  builder.CreateStore(absVal, numAlloca);
  builder.CreateBr(loopBB);

  // Loop: extract digits right to left
  builder.SetInsertPoint(loopBB);
  Value* num = builder.CreateLoad(i64Ty, numAlloca);
  Value* idx = builder.CreateLoad(i32Ty, idxAlloca);

  Value* digit = builder.CreateURem(num, ConstantInt::get(i64Ty, 10));
  Value* digitChar = builder.CreateAdd(digit, ConstantInt::get(i64Ty, '0'));
  Value* digitChar8 = builder.CreateTrunc(digitChar, i8Ty);

  Value* charPtr = builder.CreateGEP(i8Ty, buffer, idx);
  builder.CreateStore(digitChar8, charPtr);

  Value* newIdx = builder.CreateSub(idx, ConstantInt::get(i32Ty, 1));
  builder.CreateStore(newIdx, idxAlloca);
  Value* newNum = builder.CreateUDiv(num, ConstantInt::get(i64Ty, 10));
  builder.CreateStore(newNum, numAlloca);

  Value* cont = builder.CreateICmpUGT(newNum, ConstantInt::get(i64Ty, 0));
  builder.CreateCondBr(cont, loopBB, afterLoopBB);

  // After loop: check if negative
  builder.SetInsertPoint(afterLoopBB);
  builder.CreateCondBr(isNegative, addMinusBB, writeBB);

  // Add minus sign
  builder.SetInsertPoint(addMinusBB);
  Value* minusIdx = builder.CreateLoad(i32Ty, idxAlloca);
  Value* minusPtr = builder.CreateGEP(i8Ty, buffer, minusIdx);
  builder.CreateStore(ConstantInt::get(i8Ty, '-'), minusPtr);
  builder.CreateStore(builder.CreateSub(minusIdx, ConstantInt::get(i32Ty, 1)),
                      idxAlloca);
  builder.CreateBr(writeBB);

  // Write to stdout
  builder.SetInsertPoint(writeBB);
  Value* finalIdx = builder.CreateLoad(i32Ty, idxAlloca);
  Value* startIdx = builder.CreateAdd(finalIdx, ConstantInt::get(i32Ty, 1));
  Value* startPtr = builder.CreateGEP(i8Ty, buffer, startIdx);
  Value* length =
      builder.CreateSub(ConstantInt::get(i32Ty, kBufSize), startIdx);
  Value* length64 = builder.CreateZExt(length, i64Ty);
  Value* fd = ConstantInt::get(i32Ty, 1);

  emitLibcWriteInline(builder, module, fd, startPtr, length64);
  builder.CreateRetVoid();

  return func;
}

// Get or create the __sun_print_newline helper function
static Function* getOrCreatePrintNewlineHelper(llvm::Module* module,
                                               LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_print_newline");
  if (func) return func;

  FunctionType* funcType =
      FunctionType::get(Type::getVoidTy(llvmCtx), {}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_print_newline", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  AllocaInst* buffer = builder.CreateAlloca(Type::getInt8Ty(llvmCtx));
  builder.CreateStore(ConstantInt::get(Type::getInt8Ty(llvmCtx), '\n'), buffer);

  Value* fd = ConstantInt::get(Type::getInt32Ty(llvmCtx), 1);
  Value* len = ConstantInt::get(Type::getInt64Ty(llvmCtx), 1);
  emitLibcWriteInline(builder, module, fd, buffer, len);
  builder.CreateRetVoid();

  return func;
}

// Get or create the __sun_print_string helper function
// Takes an i8* (null-terminated string) and prints it
static Function* getOrCreatePrintStringHelper(llvm::Module* module,
                                              LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_print_string");
  if (func) return func;

  // Create function: void __sun_print_string(i8* %str)
  FunctionType* funcType = FunctionType::get(
      Type::getVoidTy(llvmCtx), {PointerType::getUnqual(llvmCtx)}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_print_string", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  BasicBlock* loopBB = BasicBlock::Create(llvmCtx, "strlen_loop", func);
  BasicBlock* writeBB = BasicBlock::Create(llvmCtx, "write", func);

  IRBuilder<> builder(entryBB);
  Value* strPtr = func->arg_begin();

  // Calculate string length using a loop (manual strlen)
  AllocaInst* lenAlloca = builder.CreateAlloca(Type::getInt64Ty(llvmCtx));
  builder.CreateStore(ConstantInt::get(Type::getInt64Ty(llvmCtx), 0),
                      lenAlloca);
  builder.CreateBr(loopBB);

  // strlen loop
  builder.SetInsertPoint(loopBB);
  Value* len = builder.CreateLoad(Type::getInt64Ty(llvmCtx), lenAlloca);
  Value* charPtr = builder.CreateGEP(Type::getInt8Ty(llvmCtx), strPtr, len);
  Value* ch = builder.CreateLoad(Type::getInt8Ty(llvmCtx), charPtr);
  Value* isNull =
      builder.CreateICmpEQ(ch, ConstantInt::get(Type::getInt8Ty(llvmCtx), 0));
  Value* newLen =
      builder.CreateAdd(len, ConstantInt::get(Type::getInt64Ty(llvmCtx), 1));
  builder.CreateStore(newLen, lenAlloca);
  builder.CreateCondBr(isNull, writeBB, loopBB);

  // Write to stdout
  builder.SetInsertPoint(writeBB);
  Value* finalLen = builder.CreateLoad(Type::getInt64Ty(llvmCtx), lenAlloca);
  // Subtract 1 because we incremented past the null terminator
  finalLen = builder.CreateSub(finalLen,
                               ConstantInt::get(Type::getInt64Ty(llvmCtx), 1));
  Value* fd = ConstantInt::get(Type::getInt32Ty(llvmCtx), 1);
  emitLibcWriteInline(builder, module, fd, strPtr, finalLen);
  builder.CreateRetVoid();

  return func;
}

// -------------------------------------------------------------------
// Print codegen methods
// -------------------------------------------------------------------

// Emit call to __sun_print_i32 helper
Value* CodegenVisitor::codegenPrintI32(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("print_i32 expects exactly 1 argument");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* val = codegen(*expr.getArgs()[0]);
  if (!val) return nullptr;

  if (!val->getType()->isIntegerTy(32)) {
    val = ctx.builder->CreateSExtOrTrunc(val, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreatePrintI32Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {val});
}

// Emit call to __sun_print_i64 helper (reuses i32 approach)
Value* CodegenVisitor::codegenPrintI64(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("print_i64 expects exactly 1 argument");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* val = codegen(*expr.getArgs()[0]);
  if (!val) return nullptr;

  if (!val->getType()->isIntegerTy(64)) {
    val = ctx.builder->CreateSExtOrTrunc(val, Type::getInt64Ty(llvmCtx));
  }

  Function* helper = getOrCreatePrintI64Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {val});
}

// Emit call to print f64 (simplified: prints integer part only for now)
Value* CodegenVisitor::codegenPrintF64(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("print_f64 expects exactly 1 argument");
    return nullptr;
  }

  // Simplified: convert to i32 and print
  // TODO: implement proper float printing
  LLVMContext& llvmCtx = ctx.getContext();
  Value* val = codegen(*expr.getArgs()[0]);
  if (!val) return nullptr;

  val = ctx.builder->CreateFPToSI(val, Type::getInt32Ty(llvmCtx));
  Function* helper = getOrCreatePrintI32Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {val});
}

// Emit call to __sun_print_newline helper
Value* CodegenVisitor::codegenPrintNewline() {
  LLVMContext& llvmCtx = ctx.getContext();
  Function* helper = getOrCreatePrintNewlineHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {});
}

// Emit call to __sun_print_string helper
// Supports two overloads:
//   println(str: static_ptr<u8>) - string literals (fat pointer struct)
Value* CodegenVisitor::codegenPrintString(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("println expects exactly 1 argument");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* val = codegen(*expr.getArgs()[0]);
  if (!val) return nullptr;

  // Overload 1: static_ptr<u8> (string literals)
  // Fat pointer struct { ptr, i64 } - extract the raw data pointer (element 0)
  if (val->getType()->isStructTy()) {
    val = ctx.builder->CreateExtractValue(val, 0, "str.data");
  }
  Function* helper = getOrCreatePrintStringHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {val});
}

// void __sun_print_char(i32 %scalar) — write one char to stdout as UTF-8.
//
// Branch-free: all four bytes are computed with selects and stored into a
// 4-byte buffer, then only the first `len` of them are written. A scalar
// value that is somehow out of range (only reachable through an unchecked
// _convert) is written as U+FFFD rather than as invalid UTF-8.
static Function* getOrCreatePrintCharHelper(llvm::Module* module,
                                            LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_print_char");
  if (func) return func;

  FunctionType* funcType = FunctionType::get(
      Type::getVoidTy(llvmCtx), {Type::getInt32Ty(llvmCtx)}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_print_char", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);
  llvm::Type* i32 = Type::getInt32Ty(llvmCtx);
  llvm::Type* i8 = Type::getInt8Ty(llvmCtx);
  auto k = [&](uint32_t v) { return ConstantInt::get(i32, v); };

  Value* raw = func->arg_begin();

  // Substitute U+FFFD for anything that is not a Unicode scalar value.
  Value* tooBig = builder.CreateICmpUGT(raw, k(0x10FFFF), "too_big");
  Value* isSurrogate =
      builder.CreateAnd(builder.CreateICmpUGE(raw, k(0xD800)),
                        builder.CreateICmpULE(raw, k(0xDFFF)), "is_surrogate");
  Value* c = builder.CreateSelect(builder.CreateOr(tooBig, isSurrogate),
                                  k(0xFFFD), raw, "scalar");

  Value* is1 = builder.CreateICmpULT(c, k(0x80), "is1");
  Value* is2 = builder.CreateICmpULT(c, k(0x800), "is2");
  Value* is3 = builder.CreateICmpULT(c, k(0x10000), "is3");

  Value* len = builder.CreateSelect(
      is1, k(1),
      builder.CreateSelect(is2, k(2), builder.CreateSelect(is3, k(3), k(4))),
      "len");

  // Continuation byte: 0x80 | ((c >> shift) & 0x3F)
  auto cont = [&](unsigned shift) {
    Value* v = shift ? builder.CreateLShr(c, k(shift)) : c;
    return builder.CreateOr(builder.CreateAnd(v, k(0x3F)), k(0x80));
  };
  // Leading byte: mask | (c >> shift)
  auto lead = [&](uint32_t mask, unsigned shift) {
    return builder.CreateOr(builder.CreateLShr(c, k(shift)), k(mask));
  };

  Value* b0 = builder.CreateSelect(
      is1, c,
      builder.CreateSelect(
          is2, lead(0xC0, 6),
          builder.CreateSelect(is3, lead(0xE0, 12), lead(0xF0, 18))),
      "b0");
  Value* b1 = builder.CreateSelect(
      is2, cont(0), builder.CreateSelect(is3, cont(6), cont(12)), "b1");
  Value* b2 = builder.CreateSelect(is3, cont(0), cont(6), "b2");
  Value* b3 = cont(0);

  llvm::Type* bufType = ArrayType::get(i8, 4);
  AllocaInst* buffer = builder.CreateAlloca(bufType, nullptr, "utf8");
  Value* bytes[4] = {b0, b1, b2, b3};
  for (unsigned i = 0; i < 4; ++i) {
    Value* slot = builder.CreateGEP(bufType, buffer, {k(0), k(i)});
    builder.CreateStore(builder.CreateTrunc(bytes[i], i8), slot);
  }

  emitLibcWriteInline(builder, module, ConstantInt::get(i32, 1), buffer,
                      builder.CreateZExt(len, Type::getInt64Ty(llvmCtx)));
  builder.CreateRetVoid();
  return func;
}

// _print_char(c: char) -> void
Value* CodegenVisitor::codegenPrintChar(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("_print_char expects 1 argument: (c: char)");
    return nullptr;
  }
  LLVMContext& llvmCtx = ctx.getContext();
  Value* val = codegen(*expr.getArgs()[0]);
  if (!val) return nullptr;
  Function* helper = getOrCreatePrintCharHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {val});
}

// Emit code to write raw bytes to stdout
// _print_bytes(ptr: raw_ptr<i8>, len: i64) -> void
Value* CodegenVisitor::codegenPrintBytes(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "_print_bytes expects 2 arguments: (ptr: raw_ptr<i8>, len: i64)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* ptr = codegen(*expr.getArgs()[0]);
  Value* len = codegen(*expr.getArgs()[1]);
  if (!ptr || !len) return nullptr;

  // Ensure len is i64
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, Type::getInt64Ty(llvmCtx));
  }

  // Write to stdout (fd = 1$)
  Value* fd = ConstantInt::get(Type::getInt32Ty(llvmCtx), 1);
  emitLibcWriteInline(*ctx.builder, module, fd, ptr, len);
  return ConstantInt::get(Type::getInt32Ty(llvmCtx), 0);
}
