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
/** Returns the implemented native object rule for a declared target, or fails.
 */
std::string nativeArchiveRule(const TargetProfile& target);
/** Adds producer-declared CPU and runtime requirements, rejecting unknown
 * names. Failure leaves the contract unchanged. */
void addNativeRequirements(BundleContract& contract,
                           const std::vector<std::string>& cpuFeatures,
                           const std::vector<std::string>& runtimeServices);
/** Checks ELF targets, symbol isolation, and runtime references.
 * Relocation support and application are delegated to the selected linker.
 * Machine code is trusted to obey the declared CPU and runtime requirements;
 * the compatibility checker validates the bundle's declarations separately.
 * Optional outputs collect runtime rules and external symbols for the contract.
 */
void validateNativeArchives(const std::vector<NativeArchiveInput>& archives,
                            const TargetProfile& target,
                            std::set<std::string>* runtimeServices = nullptr,
                            std::set<std::string>* linkSymbols = nullptr);
}  // namespace sun::abi
