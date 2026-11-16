/** Declares complete exception storage types for a program's trace capacity. */
#pragma once

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>

/** Generates exception storage shared by lowering and native TLS emission. */
namespace sun::codegen::exceptions {

/** Groups the generated types used for catch, propagation, and TLS storage. */
struct StorageTypes {
  llvm::StructType* value;
  llvm::StructType* record;
  llvm::StructType* thread;
};

/** Returns depth-specific structs containing inline trace arrays. Rejects
 * unsupported capacities or layouts that disagree with the native ABI.
 * Callers must allocate storage with the exception ABI's alignment.
 */
StorageTypes storageTypes(llvm::Module& module, unsigned depth);

}  // namespace sun::codegen::exceptions
