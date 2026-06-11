// moon_import.h — Configuration for importing .moon libraries with optional
// module aliasing
//
// Module aliasing allows using multiple versions of the same library by
// remapping their module names at link time. For example:
//   --moon stdlib-v1.moon:sun=sun_v1 --moon stdlib-v2.moon:sun=sun_v2
//
// This remaps all symbols from module "sun" to "sun_v1" or "sun_v2",
// allowing code to use both versions:
//   using sun_v1;  // Use old API
//   using sun_v2;  // Use new API

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace sun {

/// Configuration for importing a single .moon precompiled library
struct MoonImport {
  /// Path to the .moon file (absolute or relative)
  std::string path;

  /// Module name remapping: original_name -> aliased_name
  /// Example: {"sun" -> "sun_v1"} remaps sun::Vec to sun_v1::Vec
  std::unordered_map<std::string, std::string> moduleRemap;

  /// Create a MoonImport with no remapping
  explicit MoonImport(std::string path) : path(std::move(path)) {}

  /// Create a MoonImport with a single module remapping
  MoonImport(std::string path, const std::string& fromModule,
             const std::string& toModule)
      : path(std::move(path)), moduleRemap({{fromModule, toModule}}) {}

  /// Create a MoonImport with multiple remappings
  MoonImport(std::string path,
             std::unordered_map<std::string, std::string> remap)
      : path(std::move(path)), moduleRemap(std::move(remap)) {}

  /// Check if this import has any module remapping
  bool hasRemap() const { return !moduleRemap.empty(); }

  /// Get the aliased name for a module, or the original if not remapped
  std::string getAliasedModule(const std::string& original) const {
    auto it = moduleRemap.find(original);
    return it != moduleRemap.end() ? it->second : original;
  }
};

/// Parse a moon import specification from CLI argument
/// Format: "path.moon" or "path.moon:from=to" or
/// "path.moon:from1=to1,from2=to2" Returns nullopt if parsing fails
inline std::optional<MoonImport> parseMoonImportSpec(const std::string& spec) {
  size_t colonPos = spec.find(':');

  // No colon: simple import without remapping
  if (colonPos == std::string::npos) {
    return MoonImport(spec);
  }

  std::string path = spec.substr(0, colonPos);
  std::string remapStr = spec.substr(colonPos + 1);

  std::unordered_map<std::string, std::string> remap;

  // Parse comma-separated key=value pairs
  size_t pos = 0;
  while (pos < remapStr.size()) {
    size_t commaPos = remapStr.find(',', pos);
    std::string pair = (commaPos == std::string::npos)
                           ? remapStr.substr(pos)
                           : remapStr.substr(pos, commaPos - pos);

    size_t eqPos = pair.find('=');
    if (eqPos == std::string::npos || eqPos == 0 || eqPos == pair.size() - 1) {
      return std::nullopt;  // Invalid format
    }

    std::string from = pair.substr(0, eqPos);
    std::string to = pair.substr(eqPos + 1);
    remap[from] = to;

    pos = (commaPos == std::string::npos) ? remapStr.size() : commaPos + 1;
  }

  return MoonImport(path, std::move(remap));
}

}  // namespace sun
