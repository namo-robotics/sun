// sun_config.h — Optional per-folder compiler configuration. A folder holding
// (or above) an entrypoint file may contain a sun-config.json:
//
//   {
//     "sunPath": ["../build", "/opt/sun/libs"],
//     "pathVariables": { "LIBS": "libs" }
//   }
//
// Relative entries resolve against the config file's directory. Definitions
// here override configuration supplied from outside the folder: --path-var
// flags, language-server settings, and environment variables.

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sun {

struct SunConfig {
  static constexpr const char* kFileName = "sun-config.json";

  std::filesystem::path configDir;  // directory the file was found in
  std::vector<std::string> sunPath;  // extra library search dirs (absolute)
  std::map<std::string, std::string> pathVariables;  // values made absolute

  // The nearest sun-config.json in startDir or one of its parents; nullopt
  // when no folder on the way up has one.
  static std::optional<SunConfig> findFrom(
      const std::filesystem::path& startDir);

  // Parse one config file. Throws SunError on unreadable or malformed
  // content, wrong value types, or an unknown key.
  static SunConfig loadFile(const std::filesystem::path& file);
};

}  // namespace sun
