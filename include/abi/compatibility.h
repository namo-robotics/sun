#pragma once

#include <llvm/IR/Module.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "compatibility.pb.h"

/** Generates and checks complete Moon bundle requirements before semantic use.
 */
namespace sun::abi {
/** The serialized complete contract; its hash is independent of artifact
 * identity. */
using BundleContract = proto::BundleContract;
/** Effective machine properties used to compare targets. */
using TargetProfile = proto::TargetProfile;
/** Diagnostic producer provenance, excluded from compatibility hashing. */
using ProducerIdentity = proto::ProducerIdentity;

/** Canonically encodes required fields, excluding the hash and producer data.
 */
std::string encodeBundleContract(const BundleContract& contract);
/** Computes the domain-separated SHA-256 compatibility identifier. */
std::string hashBundleContract(const BundleContract& contract);
/** Hashes the sorted bitcode requirement set using canonical tagged integers.
 * Rejects unknown fields or requirements rather than silently excluding them.
 */
std::string hashBitcodeReaderContract(const proto::BitcodeReaderContract& contract);
/** Returns the build-time compiler and toolchain implementation identity. */
const std::string& compatibilityImplementation();
/** Returns diagnostic provenance for the compiler's loaded runtime libraries.
 */
const std::string& runtimeFingerprint();
/** Constructs effective target properties from the target machine. */
TargetProfile compatibilityTarget(const std::string& triple);
/** Describes emitted layouts, signatures and call sites without function
 * bodies. */
std::vector<std::string> describeBoundaries(const llvm::Module& module);
/** Generates requirements from emitted code and retained declarations. */
BundleContract describeContract(const llvm::Module& module);
/** Merges inherited requirements, rejecting conflicting rule implementations.
 */
void mergeContractRequirements(BundleContract& destination,
                               const BundleContract& inherited);
}  // namespace sun::abi
