#pragma once

#include <map>
#include <string>
#include <vector>

#include "abi/compatibility.h"
#include "moon.pb.h"

/** Validates correspondence between revisions without changing code ownership.
 */
namespace sun::abi {
/** Describes the declared library's semantic ABI under its target and rules.
 * Metadata must already have passed declaration and payload validation.
 */
proto::LibraryAbiSnapshot describeLibraryAbiSnapshot(
    const BundleContract& contract, const std::string& artifact,
    const std::vector<const moon::ModuleMetadata*>& modules);
/** Canonically encodes a dependency requirement, including its exact keys. */
std::string encodeLibraryAbiSnapshot(const proto::LibraryAbiSnapshot& revision);
/** Rewrites only declaration references in a temporary validation copy.
 * Literal strings and exact implementation symbols are never rewritten.
 */
void remapDeclarationReferences(
    google::protobuf::Message& message,
    const std::map<std::string, std::string>& replacements);
}  // namespace sun::abi
