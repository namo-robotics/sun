#pragma once

#include <llvm/IR/Module.h>

/** Describes CPU requirements shared by lowering and bundle validation. */
namespace sun::abi {
/** Builds a baseline probe checking AVX2/FMA hardware and operating-system
 * state. The body is kept unoptimized so import validation can reconstruct it
 * exactly.
 */
llvm::Function* createAvx2FmaProbe(llvm::Module& module);
/** Recognizes local AVX2/FMA kernels whose every use is a direct call dominated
 * by the successful edge of a reconstructed compiler probe. Escaping addresses,
 * extra CPU features, and unguarded calls remain unconditional requirements.
 */
bool hasGuardedCPURequirements(const llvm::Function& function);
}  // namespace sun::abi
