// sun_config.h — Optional per-folder compiler configuration. A folder holding
// (or above) an entrypoint file may contain a sun-config.json:
//
//   {
//     "sun_path": ["../build", "/opt/sun/libs"],
//     "path_variables": { "LIBS": "libs" },
//     "entrypoints": [
//       { "path": "stdlib/stdlib.sun", "type": "library",
//         "output_name": "build/stdlib",
//         "test_binary_name": "build/stdlib_test" }
//     ],
//     "root": true
//   }
//
// Relative entries resolve against the config file's directory. Every
// sun-config.json from the entrypoint's folder up to the filesystem root is
// merged: path variables union with the nearest definition winning, and
// search dirs and entrypoints concatenate nearest-first. "root": true stops
// the upward search at that file. Explicit --path-var flags override config
// values. Config values override project and language-server defaults and
// environment variables. Dependency directory names are reserved and cannot
// collide with ordinary path variables.
//
// The entrypoints list names the project's build products, so a config file
// can stand in for an entrypoint on the command line (`sun test
// sun-config.json`) and tools like the editor's test explorer know every
// entrypoint without scanning. The requested target (or host) selects matching
// values independently for search paths, path variables, and entrypoint fields.
// Legacy top-level target blocks can still replace complete entrypoint lists.
// Config builds also assemble named packages with their distribution resources.

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {

/** A file or directory copied into a distribution and optionally the build
 * directory. */
struct ConfigResource {
  std::string path;
  std::string destination;
};

/** A C header translated into a Sun module during compilation. */
struct ConfigCBinding {
  bool enabled = true;
  bool skipIfUnavailable = false;
  std::string header;
  std::string output;
  std::string baseDir;
  std::vector<std::string> allow;
  std::vector<std::string> clangArgs;
};

/** A selected archive, Moon file, or Git library, resolved only when used. */
struct ConfigDependency {
  bool available = true;
  bool package = false;
  std::string library;
  std::string binary;
  std::vector<ConfigDependency> selections;
  // Package IDs mapped to the entrypoints requested from this repository.
  std::map<std::string, std::vector<std::string>> gitPackages;
  std::string path;  // local artifact, or repository-relative Git source file
  std::string url;
  std::string hash;
  std::string filename;
  std::string git;
  std::string version;
  std::string config = "sun-config.json";
  std::vector<std::string> entrypoints;
};

/** An availability check for a native library or a host executable. */
struct ConfigDependencyCheck {
  /** Selects metadata-based library discovery or an executable lookup on PATH. */
  enum class Type { PkgConfig, Executable };
  Type type = Type::PkgConfig;
  std::string name;
  std::string version;
};

/** A system package requirement or a checksum-pinned Debian package. */
struct ConfigSystemDependency {
  std::string type;
  std::optional<ConfigDependencyCheck> check;
  std::vector<std::string> apt;
  std::vector<std::string> brew;
  std::string url;
  std::string hash;
  std::string path;
};

/** A tracked external build action with declared inputs and outputs. */
struct ConfigBuildStep {
  std::string name;
  std::vector<std::string> command;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::vector<std::string> prerequisites;
  std::string workingDirectory;
  std::map<std::string, std::string> environment;
};

/** A named distribution assembled from configured production artifacts. */
struct ConfigPackage {
  std::string name;
  std::string outputName;
  std::vector<std::string> entrypoints;
  std::vector<ConfigResource> resources;
};

/** Returns the canonical config target key, retaining architecture and ABI. */
std::string configTargetKey(const std::string& targetTriple);

/**
 * One build product declared by a config: an entrypoint file, what kind of
 * artifact it compiles to, and what to call the outputs. Paths are absolute
 * after parsing, except paths containing lazy variables and Git entrypoint
 * paths, which are repository-relative.
 */
struct ConfigEntrypoint {
  /** Identifies whether a configured build target produces a program or a
   * library. */
  enum class Type {
    Binary,   // an executable: the entrypoint has a main
    Library,  // a .moon bundle: no main, tests compile to the test binary
  };

  std::string name;
  std::string path;     // the entrypoint .sun file
  std::string git;      // repository URL; empty for local entrypoints
  std::string version;  // commit ID, branch, or tag for a source checkout
  std::vector<std::string> build;
  std::vector<std::string> steps;
  std::vector<std::string> compileArgs;
  std::vector<std::string> pkgConfig;
  std::vector<std::string> linkArgs;
  std::vector<ConfigResource> resources;
  std::vector<std::string> prerequisites;
  bool exported = false;
  Type type = Type::Binary;
  std::string outputName;      // resolved build artifact path
  std::string testBinaryName;  // resolved test executable path
};

/** A formatter exclusion pattern anchored to its declaring config directory. */
struct ConfigFmtIgnore {
  std::filesystem::path baseDir;
  std::string pattern;
};

/** Parsed project configuration used to build and run Sun targets. */
struct SunConfig {
  static constexpr const char* kFileName = "sun-config.json";

  std::vector<std::string> projects;
  std::vector<ConfigBuildStep> steps;
  std::vector<ConfigResource> exports;
  std::string packageId;
  bool targetSupported = true;  // selected target is allowed by supported_targets
  std::filesystem::path configFile;  // selected configuration file
  std::filesystem::path configDir;   // directory of the nearest config file
  std::vector<std::string> sunPath;  // extra library search dirs (absolute)
  std::map<std::string, std::string> pathVariables;  // values made absolute
  std::vector<ConfigEntrypoint> entrypoints;         // declared build products
  std::map<std::string, ConfigDependency> dependencies;
  std::vector<ConfigSystemDependency> systemDependencies;
  std::vector<ConfigPackage> packages;
  std::map<std::string, ConfigCBinding> cBindings;
  std::string targetTriple;  // resolved compilation triple, retaining vendor
                             // and OS version
  std::vector<ConfigFmtIgnore> fmtIgnore;  // ancestor rules first, nearest last
  bool root = false;  // stop the upward search at this file

  /**
   * The merged view of every sun-config.json in startDir and its parents
   * (nearest definitions win, search dirs and entrypoints concatenate
   * nearest-first, a "root": true file ends the walk); nullopt when no
   * folder has one.
   */
  static std::optional<SunConfig> findFrom(
      const std::filesystem::path& startDir,
      const std::string& targetTriple = "");

  /** Parse a config and select settings for the requested target or host.
   * Report malformed content, invalid target settings, and unknown keys. */
  static SunConfig loadFile(const std::filesystem::path& file,
                            const std::string& targetTriple = "");
};

}  // namespace sun::driver
