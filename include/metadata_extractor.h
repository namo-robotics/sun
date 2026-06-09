#pragma once

#include <optional>
#include <string>

#include "moon.pb.h"

namespace sun {

/// Parse a source file and extract module metadata without full compilation.
/// Computes a SHA-256 hash of the file contents and stores it in the metadata.
/// Returns a protobuf ModuleMetadata with serialized AST nodes for classes,
/// interfaces, and functions. Generic function/method bodies are included
/// for later instantiation; non-generic bodies are omitted.
/// @param filename Path to the .sun source file
/// @return Extracted metadata, or nullopt on failure
std::optional<moon::ModuleMetadata> extractMetadataFromFile(
    const std::string& filename);

}  // namespace sun
