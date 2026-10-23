// moon_builder.h — Build a .moon library bundle from an entrypoint file.
//
// One place for the whole `sun --emit-moon` pipeline, shared by the CLI and
// the tests: resolve the entrypoint's manifest, synthesize modules for its
// `protos:`, extract exportable metadata (from the .sun files and from the
// synthesized proto modules), compile everything together, and write the
// bundle.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "moon_bundling/moon_import.h"
#include "moon_bundling/native_archive.h"

/** Declares syntax-tree types consumed by metadata extraction. */
namespace sun::ast {
class BlockExprAST;
}
/** Declares semantic analysis used to validate bundle exports. */
namespace sun::semantic_analysis {
class SemanticAnalyzer;
}

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {

/** Writes compiled modules and native archives into Moon bundles. */
class MoonWriter;

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
  // Resolved manifest archives and their declared requirements; carried with
  // their symbols renamed under the bundle's hash.
  std::vector<NativeArchive> nativeArchives;
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

 private:
  /** Prepared input files, generated sources, archive bytes, and build hashes.
   */
  struct LoadedInputs;
  /** Native symbol renames and unresolved references. */
  struct NativeSymbols;
  /** Validated export metadata and module names. */
  struct ExportMetadata;
  /** Compiled code and the metadata whose lifetime it depends on. */
  struct CompiledBundle;

  /** Resolves and loads inputs, generates sources, and computes build hashes.
   */
  static LoadedInputs loadInputs(const std::string& entrypoint,
                                 const MoonBuildOptions& options);
  /** Checks whether the existing output matches the inputs and needs no
   * rebuild. */
  static bool isUpToDate(const std::string& inputHash,
                         const std::filesystem::path& outputPath,
                         const MoonBuildOptions& options);
  /** Adds cached exports and inherited archives to the supplied input report.
   */
  static MoonBuildReport readExistingReport(
      const std::filesystem::path& bundlePath,
      const std::string& ownArchiveSetHash, MoonBuildReport report);
  /** Computes native symbol renames and warns about duplicate definitions. */
  static NativeSymbols prepareNativeSymbols(const LoadedInputs& inputs);
  /** Returns validated exports with bundle-local source identifiers. */
  static ExportMetadata collectMetadata(
      const sun::ast::BlockExprAST& program,
      sun::semantic_analysis::SemanticAnalyzer& analyzer,
      const std::string& inputHash, uint32_t staticInitOrder);
  /** Compiles prepared inputs and returns the driver and its export metadata.
   */
  static CompiledBundle compile(const LoadedInputs& inputs,
                                const NativeSymbols& symbols,
                                const std::string& entrypoint,
                                const MoonBuildOptions& options);
  /** Adds archives to the writer and returns inherited archive names. */
  static std::vector<std::string> packageNativeArchives(
      MoonWriter& writer, const LoadedInputs& inputs,
      const NativeSymbols& symbols,
      const std::vector<std::string>& inheritedPaths);
  /** Writes the bundle's JSON representation into the debug directory. */
  static void writeDebugJson(const std::filesystem::path& outputPath,
                             const std::filesystem::path& debugFolder);
  /** Writes compiled code and archives, returning inherited archive names. */
  static std::vector<std::string> writeBundle(
      const LoadedInputs& inputs, const NativeSymbols& symbols,
      CompiledBundle& compiled, const std::filesystem::path& outputPath,
      const MoonBuildOptions& options);
};

}  // namespace sun::moon_bundling
