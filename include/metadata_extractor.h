#pragma once

#include <optional>
#include <string>

#include "moon.h"

namespace sun {

/// Parse a source file and extract module metadata without full compilation.
/// Computes a SHA-256 hash of the file contents and stores it in the metadata.
/// @param filename Path to the .sun source file
/// @return Extracted metadata, or nullopt on failure
std::optional<ModuleMetadata> extractMetadataFromFile(
    const std::string& filename);

}  // namespace sun
