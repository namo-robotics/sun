#pragma once

#include <string>
#include <vector>

#include "moon.pb.h"

/** Describes the declarations and retained bodies interpreted by an importer.
 */
namespace sun::abi {
/** Canonically encodes semantic metadata, excluding diagnostic and container
 * fields. */
std::string describeSemantics(const moon::ModuleMetadata& metadata);
/** Checks declaration ownership and retained references against bundle and
 * compiler-provided identities before import. */
void validateDeclarationGraph(
    const std::vector<const moon::ModuleMetadata*>& modules);
}  // namespace sun::abi
