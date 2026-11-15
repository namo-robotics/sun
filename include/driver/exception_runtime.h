/** Connects generated exception storage to the shared native runtime. */
#pragma once
#include <memory>
#include <string>

/** Forward declarations for the native object generator. */
namespace llvm {
class Module;
}

/** Prepares exception support before generated code can execute. */
namespace sun::driver {
/** Defines the depth constant and TLS block when emitting a native object. */
bool defineExceptionRuntimeStorage(llvm::Module& module, std::string& error);
/** Links isolated JIT storage against the runtime archive; null selects the
 * host runtime. */
std::shared_ptr<void> loadExceptionRuntime(unsigned depth);
}  // namespace sun::driver
