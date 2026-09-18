#pragma once

#include <llvm/Support/FileSystem.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "support/target_os.h"

namespace sun::support {

/// Centralized SUN_PATH environment variable handling.
/// SUN_PATH is a colon-separated list of directories used to resolve imports.
class SunPath {
 public:
  /// Directories added with --lib-path (or by an embedding tool). Searched
  /// ahead of SUN_PATH, so an explicit flag wins over the environment.
  static std::vector<std::filesystem::path>& extraPaths() {
    static std::vector<std::filesystem::path> paths;
    return paths;
  }

  /// Add a directory to search for .moon bundles
  static void addSearchPath(const std::filesystem::path& dir) {
    auto& paths = extraPaths();
    if (std::find(paths.begin(), paths.end(), dir) == paths.end()) {
      paths.push_back(dir);
    }
  }

  /// Get the directories imports resolve against: --lib-path first, then
  /// the SUN_PATH environment variable.
  static std::vector<std::filesystem::path> getPaths() {
    std::vector<std::filesystem::path> paths = extraPaths();
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

  /** Find an installed bundle, preferring directories for its target. */
  static std::filesystem::path resolveInstalledBundle(
      const std::string& name, const std::string& targetTriple) {
    const std::filesystem::path path(name);
    std::vector<std::string> targets;
    if (!path.has_parent_path() && path.extension().string() == ".moon") {
      auto target = resolvedTargetTriple(targetTriple);
      auto add = [&](const std::string& triple) {
        if (!triple.empty() && std::find(targets.begin(), targets.end(),
                                         triple) == targets.end()) {
          targets.push_back(triple);
        }
      };
      add(targetTriple);
      add(target.str());
      if (target.isOSLinux()) {
        const auto arch = target.getArchName().str();
        add(arch + "-linux-" + target.getEnvironmentName().str());
        if (target.getEnvironment() == llvm::Triple::GNU ||
            target.getEnvironment() == llvm::Triple::Musl) {
          add(arch + "-linux-gnu");
          add(arch + "-linux-musl");
        }
      } else if (target.isOSDarwin()) {
        add((target.getArch() == llvm::Triple::aarch64
                 ? std::string("arm64")
                 : target.getArchName().str()) +
            "-apple-darwin");
      }
    }
    for (const auto& dir : systemInstallDirs()) {
      for (const auto& target : targets) {
        auto candidate = dir / target / path;
        if (std::filesystem::exists(candidate)) return candidate;
      }
      auto candidate = dir / path;
      if (std::filesystem::exists(candidate)) return candidate;
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

  /// The system-wide directories installed bundles live in, existing ones
  /// only. Two directories relative to the compiler binary come first, so a
  /// relocated or Homebrew-prefixed install finds its own bundles; then the
  /// fixed prefixes the Debian package (/usr) and Homebrew
  /// (/opt/homebrew, /usr/local — macOS keeps /usr/lib read-only) install
  /// to.
  static std::vector<std::filesystem::path> systemInstallDirs() {
    std::vector<std::filesystem::path> dirs;
    auto addIfExists = [&](const std::filesystem::path& dir) {
      if (std::filesystem::exists(dir) &&
          std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) {
        dirs.push_back(dir);
      }
    };

    std::string exe = llvm::sys::fs::getMainExecutable(
        "sun", reinterpret_cast<void*>(&SunPath::systemInstallDirs));
    if (!exe.empty()) {
      auto prefix = std::filesystem::path(exe).parent_path().parent_path();
      addIfExists(prefix / "lib" / "sun");
      addIfExists(prefix / "share" / "sun" / "stdlib");
    }
    for (const char* prefix : {"/usr", "/usr/local", "/opt/homebrew"}) {
      addIfExists(std::filesystem::path(prefix) / "lib" / "sun");
      addIfExists(std::filesystem::path(prefix) / "share" / "sun" / "stdlib");
    }
    return dirs;
  }
};

}  // namespace sun::support
