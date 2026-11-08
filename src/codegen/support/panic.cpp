/** Emits diagnostics and process termination for unrecoverable failures. */

#include "codegen/support/panic.h"

#include "codegen/intrinsics/libc.h"

/** Provides panic emission shared by runtime checks and explicit panic calls. */
namespace sun::codegen::support {

void emitPanic(llvm::IRBuilder<>& builder, llvm::Module* module,
               llvm::Value* data, llvm::Value* length) {
  constexpr llvm::StringLiteral prefix = "panic: ";
  auto write = sun::codegen::intrinsics::write(module);
  builder.CreateCall(write, {builder.getInt32(2),
                            builder.CreateGlobalString(prefix),
                            builder.getInt64(prefix.size())});
  builder.CreateCall(write, {builder.getInt32(2), data, length});
  builder.CreateCall(write, {builder.getInt32(2),
                            builder.CreateGlobalString("\n"),
                            builder.getInt64(1)});
  auto* call = builder.CreateCall(sun::codegen::intrinsics::abort(module));
  call->setDoesNotReturn();
  call->setDoesNotThrow();
  builder.CreateUnreachable();
}

void emitPanic(llvm::IRBuilder<>& builder, llvm::Module* module,
               llvm::StringRef message) {
  emitPanic(builder, module, builder.CreateGlobalString(message),
            builder.getInt64(message.size()));
}

}  // namespace sun::codegen::support
