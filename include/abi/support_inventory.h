#pragma once

#include <map>
#include <string>

#include "compatibility.pb.h"

/** Publishes implemented compatibility rules independently of producer age. */
namespace sun::abi {
/** Returns the immutable build-validated compiler support descriptor. */
const proto::CompilerAbiSupport& compilerAbiSupport();
/** Returns supported rule implementations; this inventory never authorizes a
 * payload. */
std::map<std::string, std::string> supportedRules();
/** Recognizes well-formed advisory fingerprints for implemented experimental rules. */
bool isExperimentalFingerprint(const std::string& name,
                               const std::string& implementation);
/** Compares rule identities, allowing experimental fingerprints and bitcode
 * hashes to differ. Bitcode still requires payload and ABI validation. */
bool compatibleRuleImplementations(const std::string& name,
                                   const std::string& required,
                                   const std::string& supported);
/** Resolves the build-selected emission default to its wire implementation
 * identity. */
std::string emissionImplementation(const std::string& name);
/** Returns the experimental support inventory and producer identity as release
 * JSON. */
std::string compatibilitySupportJson();
}  // namespace sun::abi
