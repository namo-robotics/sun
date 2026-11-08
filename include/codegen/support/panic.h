/** Declares diagnostic abort emission shared by code generators. */
#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

/** Provides panic emission shared by runtime checks and explicit panic calls. */
namespace sun::codegen::support {

/** Writes a diagnostic and aborts without allocating or unwinding. */
void emitPanic(llvm::IRBuilder<>& builder, llvm::Module* module,
               llvm::Value* data, llvm::Value* length);

/** Writes a fixed diagnostic and aborts without unwinding. */
void emitPanic(llvm::IRBuilder<>& builder, llvm::Module* module,
               llvm::StringRef message);

}  // namespace sun::codegen::support
