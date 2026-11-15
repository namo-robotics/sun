/** Builds source indexes and registers them with the in-process runtime. */
#pragma once
#include <memory>

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

/** Forward declarations for the source-index compilation hooks. */
namespace llvm {
class Module;

namespace orc {
class ObjectLinkingLayer;
}
}  // namespace llvm

/** Prepares immutable metadata; reporting itself never parses DWARF or
 * allocates. */
namespace sun::driver {
/** Selects the runtime instance that owns a JIT module's source indexes. */
struct ExceptionSourceCallbacks {
  void (*add)(void*) noexcept = nullptr;
  void (*remove)(void*) noexcept = nullptr;
};

/** Reserves an index whose function pointers receive normal linker relocations.
 */
void prepareExceptionSources(llvm::Module& module, bool jit);
/** Packs the object's DWARF locations into its reserved index before linking.
 */
llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> indexExceptionSources(
    llvm::Module& module, llvm::MemoryBufferRef object);
/** Registers and removes JIT indexes with the same lifetime as their code. */
void addExceptionSourceRegistration(
    llvm::orc::ObjectLinkingLayer& layer,
    std::shared_ptr<ExceptionSourceCallbacks> callbacks);
}  // namespace sun::driver
