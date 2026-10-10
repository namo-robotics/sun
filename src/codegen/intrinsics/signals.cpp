/** Emits the small native signal boundary used by std.signal. */
#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics_generator.h"
#include "support/error.h"
#include "support/target_os.h"

/** Implements compiler-owned native operations. */
namespace sun::codegen::intrinsics {

llvm::Value* IntrinsicsGenerator::codegenSignalIntrinsic(
    const sun::ast::CallExprAST& expr, bool handler) {
  if (!expr.getArgs().empty()) {
    sun::support::logAndThrowError("signal intrinsics take no arguments");
  }
  const auto triple =
      sun::support::resolvedTargetTriple(module->getTargetTriple());
  if ((!triple.isOSLinux() && !triple.isOSDarwin()) ||
      (triple.getArch() != llvm::Triple::x86_64 &&
       triple.getArch() != llvm::Triple::aarch64)) {
    sun::support::logAndThrowError(
        "signal subscriptions require Linux or macOS on x86-64 or AArch64");
  }
  auto& context = module->getContext();
  auto* i32 = llvm::Type::getInt32Ty(context);
  auto* storage = llvm::ArrayType::get(i32, 4);
  auto* state = module->getGlobalVariable("__sun_signal_state", true);
  if (!state) {
    state = new llvm::GlobalVariable(
        *module, storage, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantAggregateZero::get(storage), "__sun_signal_state");
    state->setAlignment(llvm::Align(4));
  }
  if (!handler) return state;
  if (auto* existing = module->getFunction("__sun_signal_handler"))
    return existing;

  // Use a separate builder: this callback must not inherit Sun debug locations,
  // closure handling, cleanup, allocation, or exception machinery.
  auto* fn = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i32}, false),
      llvm::GlobalValue::InternalLinkage, "__sun_signal_handler", module);
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", fn));
  auto* notify = llvm::BasicBlock::Create(context, "notify", fn);
  auto* write = llvm::BasicBlock::Create(context, "write", fn);
  auto* check = llvm::BasicBlock::Create(context, "check", fn);
  auto* done = llvm::BasicBlock::Create(context, "done", fn);
  auto* gate = b.CreateConstInBoundsGEP2_32(storage, state, 0, 0);
  auto* fd = b.CreateConstInBoundsGEP2_32(storage, state, 0, 1);
  auto* pending = b.CreateConstInBoundsGEP2_32(storage, state, 0, 3);
  auto* ptr = llvm::PointerType::getUnqual(context);
  auto errnoFn = module->getOrInsertFunction(
      triple.isOSDarwin() ? "__error" : "__errno_location",
      llvm::FunctionType::get(ptr, {}, false));
  auto* errnoPtr = b.CreateCall(errnoFn);
  auto* savedErrno = b.CreateLoad(i32, errnoPtr);
  auto* byte = b.CreateAlloca(b.getInt8Ty());
  b.CreateStore(b.getInt8(1), byte);
  // The low bit enables access; the other bits count in-flight handlers.
  // Disabled handlers still count, but never access the descriptor or pending
  // bits.
  auto* previous =
      b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, gate, b.getInt32(2),
                        llvm::Align(4), llvm::AtomicOrdering::AcquireRelease);
  b.CreateCondBr(
      b.CreateICmpNE(b.CreateAnd(previous, b.getInt32(1)), b.getInt32(0)),
      notify, done);
  b.SetInsertPoint(notify);
  auto* bit = b.CreateSelect(b.CreateICmpEQ(fn->getArg(0), b.getInt32(2)),
                             b.getInt32(1), b.getInt32(2));
  auto* alreadyPending =
      b.CreateAtomicRMW(llvm::AtomicRMWInst::Or, pending, bit, llvm::Align(4),
                        llvm::AtomicOrdering::AcquireRelease);
  auto* descriptor = b.CreateLoad(i32, fd);
  auto writer = module->getOrInsertFunction(
      "write", llvm::FunctionType::get(b.getInt64Ty(),
                                       {i32, ptr, b.getInt64Ty()}, false));
  // One queued byte is enough until the consumer takes the pending bits.
  b.CreateCondBr(b.CreateICmpEQ(alreadyPending, b.getInt32(0)), write, done);
  b.SetInsertPoint(write);
  auto* result = b.CreateCall(writer, {descriptor, byte, b.getInt64(1)});
  b.CreateCondBr(b.CreateICmpEQ(result, b.getInt64(-1)), check, done);
  b.SetInsertPoint(check);
  // EINTR retries; EAGAIN means a wakeup is already queued. Pending bits
  // survive.
  b.CreateCondBr(b.CreateICmpEQ(b.CreateLoad(i32, errnoPtr), b.getInt32(4)),
                 write, done);
  b.SetInsertPoint(done);
  b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, gate, b.getInt32(2),
                    llvm::Align(4), llvm::AtomicOrdering::AcquireRelease);
  b.CreateStore(savedErrno, errnoPtr);
  b.CreateRetVoid();
  return fn;
}

}  // namespace sun::codegen::intrinsics
