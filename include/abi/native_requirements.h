#pragma once

#include <llvm/Support/MemoryBufferRef.h>

#include <set>
#include <vector>

#include "abi/compatibility.h"

/** Checks native archive structure and referenced runtime services. */
namespace sun::abi {
/** Borrows an embedded archive and the namespace assigned to its exported
 * symbols. */
struct NativeArchiveInput {
  llvm::MemoryBufferRef bytes;
  std::string symbolPrefix;
};
/** Checks ELF targets, symbol isolation, and runtime references.
 * Relocation support and application are delegated to the selected linker.
 * Machine code is trusted to obey the declared CPU and runtime requirements;
 * the compatibility checker validates the bundle's declarations separately. */
void validateNativeArchives(const std::vector<NativeArchiveInput>& archives,
                            const TargetProfile& target,
                            std::set<std::string>* runtimeServices = nullptr);
}  // namespace sun::abi
