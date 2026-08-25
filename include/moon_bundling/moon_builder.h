// moon_builder.h — Build a .moon library bundle from an entrypoint file.
//
// One place for the whole `sun --emit-moon` pipeline, shared by the CLI and
// the tests: resolve the entrypoint's manifest, synthesize modules for its
// `protos:`, extract exportable metadata (from the .sun files and from the
// synthesized proto modules), compile everything together, and write the
// bundle.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "moon_bundling/moon_import.h"

namespace sun {

struct MoonBuildOptions {
  std::string targetTriple;            // empty = host
  bool debugInfo = false;              // -g
  bool dumpProtoSun = false;           // print synthesized proto source
  std::vector<MoonImport> extraMoons;  // CLI --moon imports
};

// What went into a bundle (for logging / assertions)
struct MoonBuildReport {
  std::vector<std::string> sunFiles;  // compiled .sun files (entrypoint first)
  std::vector<std::string> protoFiles;    // synthesized .proto schemas
  std::vector<MoonImport> moonImports;    // bundles linked against
  std::vector<std::string> modules;       // exported module names (dotted)
  std::vector<std::string> archiveFiles;  // native archives in the bundle
};

class MoonBuilder {
 public:
  // Build `outputPath` from `entrypoint`. Throws SunError on any failure
  // (manifest, proto import, compilation, bundle write).
  static MoonBuildReport build(const std::string& entrypoint,
                               const std::filesystem::path& outputPath,
                               const MoonBuildOptions& options = {});

  // Default output path for an entrypoint: <entrypoint without .sun>.moon
  static std::filesystem::path defaultOutputPath(const std::string& entrypoint);
};

}  // namespace sun
