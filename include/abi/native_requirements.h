#pragma once

#include <llvm/Support/MemoryBufferRef.h>
#include <vector>
#include <set>
#include "abi/compatibility.h"

/** Reconstructs supported platform and runtime requirements from native payloads. */
namespace sun::abi {
/** Borrows an embedded archive and the namespace assigned to its exported symbols. */
struct NativeArchiveInput {
  llvm::MemoryBufferRef bytes;
  std::string symbolPrefix;
};
/** Rejects native code outside the experimental baseline x86-64 ELF profile. */
void validateNativeArchives(const std::vector<NativeArchiveInput>& archives,
                            const TargetProfile& target,
                            std::set<std::string>* runtimeServices = nullptr);
}
