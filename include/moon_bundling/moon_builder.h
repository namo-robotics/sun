// moon_builder.h — Build a .moon library bundle from an entrypoint file.
//
// One place for the whole `sun --emit-moon` pipeline, shared by the CLI and
// the tests: resolve the entrypoint's manifest, synthesize modules for its
// `protos:`, extract exportable metadata (from the .sun files and from the
// synthesized proto modules), compile everything together, and write the
// bundle.

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "moon_bundling/moon_import.h"

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {

/** Inputs and build settings for producing a Moon library. */
struct MoonBuildOptions {
  std::string targetTriple;   // empty = host
  bool debugMode = false;     // --debug: compiler artifacts and moon.json
  bool debugInfo = false;     // -g
  bool optimize = true;       // disabled by -O0
  bool dumpProtoSun = false;  // print synthesized proto source
  std::vector<MoonImport> extraMoons;  // CLI --moon imports
  // Rebuild even when the bundle on disk was made from the same inputs
  // (--force-rebuild)
  bool forceRebuild = false;
  // Called once the build is known to go ahead, before anything is compiled,
  // so a caller can announce it. Not called for a skipped build.
  std::function<void()> onBuildStart;
};

/**
 * What went into a bundle (for logging / assertions)
 */
struct MoonBuildReport {
  std::vector<std::string> sunFiles;  // compiled .sun files (entrypoint first)
  std::vector<std::string> protoFiles;  // synthesized .proto schemas
  std::vector<MoonImport> moonImports;  // bundles linked against
  std::vector<std::string> modules;     // exported module names (dotted)
  // Native archives the manifest's own `archives:` named (resolved paths);
  // carried with their symbols renamed under the bundle's hash
  std::vector<std::string> archiveFiles;
  // File names of archives taken over from imported bundles whose code was
  // linked in, so the bundle stays self-contained for its importers
  std::vector<std::string> inheritedArchives;
  // True when the bundle on disk already recorded the same input hash, so
  // nothing was compiled or written
  bool upToDate = false;
};

/** Compiles source modules and packages them into a reusable Moon library. */
class MoonBuilder {
 public:
  /**
   * Build `outputPath` from `entrypoint`. A bundle already there and built
   * from the same inputs is left alone (see input_hash.h) unless
   * options.forceRebuild or debug output asks for the work anyway.
   * Throws SunError on manifest, proto import, compilation, bundle write,
   * or debug output failure.
   */
  static MoonBuildReport build(const std::string& entrypoint,
                               const std::filesystem::path& outputPath,
                               const MoonBuildOptions& options = {});

  /**
   * Default output path for an entrypoint: <entrypoint without .sun>.moon
   */
  static std::filesystem::path defaultOutputPath(const std::string& entrypoint);
};

}  // namespace sun::moon_bundling
