// sun_config.h — Optional per-folder compiler configuration. A folder holding
// (or above) an entrypoint file may contain a sun-config.json:
//
//   {
//     "sunPath": ["../build", "/opt/sun/libs"],
//     "pathVariables": { "LIBS": "libs" },
//     "root": true
//   }
//
// Relative entries resolve against the config file's directory. Every
// sun-config.json from the entrypoint's folder up to the filesystem root is
// merged: path variables union with the nearest definition winning, and
// search dirs concatenate nearest-first. "root": true stops the upward
// search at that file. Definitions here override configuration supplied
// from outside the folders: --path-var flags, language-server settings, and
// environment variables.

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sun {

struct SunConfig {
  static constexpr const char* kFileName = "sun-config.json";

  std::filesystem::path configDir;  // directory of the nearest config file
  std::vector<std::string> sunPath;  // extra library search dirs (absolute)
  std::map<std::string, std::string> pathVariables;  // values made absolute
  bool root = false;  // stop the upward search at this file

  // The merged view of every sun-config.json in startDir and its parents
  // (nearest definitions win, search dirs concatenate nearest-first, a
  // "root": true file ends the walk); nullopt when no folder has one.
  static std::optional<SunConfig> findFrom(
      const std::filesystem::path& startDir);

  // Parse one config file. Throws SunError on unreadable or malformed
  // content, wrong value types, or an unknown key.
  static SunConfig loadFile(const std::filesystem::path& file);
};

}  // namespace sun
