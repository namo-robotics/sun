#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "moon/moon.h"

namespace sun {

/// Global cache for precompiled .moon bundles
/// Thread-safe singleton for discovering and loading precompiled libraries
class LibraryCache {
 public:
  /// Get the singleton instance
  static LibraryCache& instance();

  /// Add a directory to search for .moon files
  /// @param path Directory containing .moon bundles
  void addSearchPath(const std::filesystem::path& path);

  /// Add a specific .moon bundle file
  /// @param bundlePath Path to a .moon file
  void addBundle(const std::filesystem::path& bundlePath);

  /// Initialize from environment
  /// Loads lib/ and build/ subdirectories for each SUN_PATH entry
  void initFromEnvironment();

  /// Check if a precompiled module exists
  /// @param moduleKey The module key (source hash)
  bool hasModule(const std::string& moduleKey);

  /// Get metadata for a module
  /// @param moduleKey The module key (source hash)
  /// @return Metadata, or nullptr if not found
  const moon::ModuleMetadata* getMetadata(const std::string& moduleKey);

  /// Load a module's LLVM bitcode
  /// @param moduleKey The module key (source hash)
  /// @param context LLVM context to create module in
  /// @return LLVM module, or nullptr if not found/failed
  std::unique_ptr<llvm::Module> loadModule(const std::string& moduleKey,
                                           llvm::LLVMContext& context);

  /// Identity of the bitcode region backing a module; modules sharing a code
  /// image share it. Empty if the module is unknown.
  std::string getBitcodeId(const std::string& moduleKey);

  /// Get all search paths
  const std::vector<std::filesystem::path>& getSearchPaths() const;

  /// Preload all bundles from search paths
  /// Call once at startup for fastest subsequent access
  void preloadAll();

  /// Clear all cached data
  void clear();

  /// Check if initialized
  bool isInitialized() const { return initialized_; }

  /// Find the bundle containing a module (for error reporting)
  MoonReader* findBundleForModule(const std::string& moduleKey);

  /// Set the compilation target. When several discovered bundles claim the
  /// same module (e.g. the host build/stdlib.moon and the cross
  /// build/aarch64-linux-gnu/stdlib.moon), the one compiled for this target
  /// wins. Empty means the host.
  void setTargetTriple(const std::string& triple);

  /// The compilation target set above (empty = host).
  const std::string& getTargetTriple() const { return targetTriple_; }

 private:
  LibraryCache() = default;
  LibraryCache(const LibraryCache&) = delete;
  LibraryCache& operator=(const LibraryCache&) = delete;

  /// Discover .moon files in search paths
  void discoverBundles();

  /// Pick among bundles claiming the same module. Preference: matching the
  /// target's architecture outranks everything (the parser opened an
  /// arch-correct bundle, and the linker must agree with it); explicit
  /// addBundle() registration breaks ties among same-arch candidates. A
  /// wrong pick is still caught by the linker's triple check.
  MoonReader* selectBundle(
      const std::vector<MoonReader*>& candidates) const;

  std::vector<std::filesystem::path> searchPaths_;
  std::vector<std::unique_ptr<MoonReader>> bundles_;
  // All bundles claiming each module key; target selection happens at lookup
  std::unordered_map<std::string, std::vector<MoonReader*>> moduleToBundle_;
  // Bundles registered explicitly (by resolved import path) rather than by
  // directory discovery
  std::set<MoonReader*> pinnedBundles_;
  std::string targetTriple_;  // empty = host
  mutable std::mutex mutex_;
  bool initialized_ = false;
  bool discovered_ = false;
};

}  // namespace sun
