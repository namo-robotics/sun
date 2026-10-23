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
/** Resolves the build-selected emission default to its wire implementation
 * identity. */
std::string emissionImplementation(const std::string& name);
/** Returns the experimental support inventory and producer identity as release
 * JSON. */
std::string compatibilitySupportJson();
}  // namespace sun::abi
