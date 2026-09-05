#pragma once

#include <optional>
#include <string>
#include <vector>

#include "moon.pb.h"

class BlockExprAST;
class SemanticAnalyzer;

namespace sun {

/** Export the bundle's own declarations from the successfully analyzed program.
 */
std::vector<moon::ModuleMetadata> extractAnalyzedMetadata(
    const BlockExprAST& program, SemanticAnalyzer& analyzer,
    const std::string& bundleHash);

/// Parse a source file and extract module metadata without full compilation.
/// Computes a SHA-256 hash of the file contents and stores it in the metadata.
/// Returns a protobuf ModuleMetadata with serialized AST nodes for classes,
/// interfaces, and functions. Generic function/method bodies are included
/// for later instantiation; non-generic bodies are omitted.
/// @param filename Path to the .sun source file
/// @return Extracted metadata, or nullopt on failure
std::optional<moon::ModuleMetadata> extractMetadataFromFile(
    const std::string& filename);

/// Same as extractMetadataFromFile for source text that has no file (e.g. a
/// module synthesized from a .proto schema). `displayName` labels the
/// metadata; `baseDir` resolves relative imports.
std::optional<moon::ModuleMetadata> extractMetadataFromSource(
    const std::string& source, const std::string& displayName,
    const std::string& baseDir = "");

/// One ModuleMetadata per module declared in the file (nested modules are
/// exported under their dotted path, e.g. "namo.telemetry"), plus one for
/// file-level definitions if any. The single-result variants above return
/// the first entry.
std::optional<std::vector<moon::ModuleMetadata>> extractAllMetadataFromFile(
    const std::string& filename);
std::optional<std::vector<moon::ModuleMetadata>> extractAllMetadataFromSource(
    const std::string& source, const std::string& displayName,
    const std::string& baseDir = "");

}  // namespace sun
