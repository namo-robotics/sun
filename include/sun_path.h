#pragma once

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace sun {

/// Centralized SUN_PATH environment variable handling.
/// SUN_PATH is a colon-separated list of directories used to resolve imports.
class SunPath {
 public:
  /// Get the parsed list of directories from the SUN_PATH environment variable.
  static std::vector<std::filesystem::path> getPaths() {
    std::vector<std::filesystem::path> paths;
    const char* env = std::getenv("SUN_PATH");
    if (!env || std::strlen(env) == 0) return paths;

    std::string pathList(env);
    std::istringstream stream(pathList);
    std::string dir;
    while (std::getline(stream, dir, ':')) {
      if (!dir.empty()) {
        paths.emplace_back(dir);
      }
    }
    return paths;
  }

  /// Resolve a relative path against SUN_PATH directories.
  /// Returns the first existing match, or empty path if not found.
  static std::filesystem::path resolve(const std::string& relativePath) {
    for (const auto& dir : getPaths()) {
      auto candidate = dir / relativePath;
      if (std::filesystem::exists(candidate)) {
        return candidate;
      }
    }
    return {};
  }

  /// Get library search paths derived from SUN_PATH (lib/ and build/ subdirs).
  static std::vector<std::filesystem::path> getLibrarySearchPaths() {
    std::vector<std::filesystem::path> searchPaths;
    for (const auto& base : getPaths()) {
      auto libPath = base / "lib";
      auto buildPath = base / "build";
      if (std::filesystem::exists(libPath)) {
        searchPaths.push_back(libPath);
      }
      if (std::filesystem::exists(buildPath)) {
        searchPaths.push_back(buildPath);
      }
    }
    return searchPaths;
  }

  /// Ensure SUN_PATH is set (to cwd if not already). Used by tests.
  static void ensureSet() {
    if (!std::getenv("SUN_PATH")) {
      auto cwd = std::filesystem::current_path().string();
      setenv("SUN_PATH", cwd.c_str(), 0);
    }
  }
};

}  // namespace sun
