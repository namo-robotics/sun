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
// the upward search at that file. Definitions here override configuration
// supplied from outside the folders: --path-var flags, language-server
// settings, and environment variables.
//
// The entrypoints list names the project's build products, so a config file
// can stand in for an entrypoint on the command line (`sun test
// sun-config.json`) and tools like the editor's test explorer know every
// entrypoint without scanning.

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sun {

// One build product declared by a config: an entrypoint file, what kind of
// artifact it compiles to, and what to call the outputs. Paths are absolute
// after parsing.
struct ConfigEntrypoint {
  enum class Type {
    Binary,   // an executable: the entrypoint has a main
    Library,  // a .moon bundle: no main, tests compile to the test binary
  };

  std::string path;            // the entrypoint .sun file
  Type type = Type::Binary;
  std::string outputName;      // empty: derived from the entrypoint's name
  std::string testBinaryName;  // empty: outputName + "_test"
};

struct SunConfig {
  static constexpr const char* kFileName = "sun-config.json";

  std::filesystem::path configDir;   // directory of the nearest config file
  std::vector<std::string> sunPath;  // extra library search dirs (absolute)
  std::map<std::string, std::string> pathVariables;  // values made absolute
  std::vector<ConfigEntrypoint> entrypoints;  // declared build products
  bool root = false;  // stop the upward search at this file

  // The merged view of every sun-config.json in startDir and its parents
  // (nearest definitions win, search dirs and entrypoints concatenate
  // nearest-first, a "root": true file ends the walk); nullopt when no
  // folder has one.
  static std::optional<SunConfig> findFrom(
      const std::filesystem::path& startDir);

  // Parse one config file. Throws SunError on unreadable or malformed
  // content, wrong value types, or an unknown key.
  static SunConfig loadFile(const std::filesystem::path& file);
};

}  // namespace sun
