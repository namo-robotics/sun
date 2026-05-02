#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "moon.h"

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
  const ModuleMetadata* getMetadata(const std::string& moduleKey);

  /// Load a module's LLVM bitcode
  /// @param moduleKey The module key (source hash)
  /// @param context LLVM context to create module in
  /// @return LLVM module, or nullptr if not found/failed
  std::unique_ptr<llvm::Module> loadModule(const std::string& moduleKey,
                                           llvm::LLVMContext& context);

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
  SunLibReader* findBundleForModule(const std::string& moduleKey);

 private:
  LibraryCache() = default;
  LibraryCache(const LibraryCache&) = delete;
  LibraryCache& operator=(const LibraryCache&) = delete;

  /// Discover .moon files in search paths
  void discoverBundles();

  std::vector<std::filesystem::path> searchPaths_;
  std::vector<std::unique_ptr<SunLibReader>> bundles_;
  std::unordered_map<std::string, SunLibReader*> moduleToBundle_;  // cache
  mutable std::mutex mutex_;
  bool initialized_ = false;
  bool discovered_ = false;
};

}  // namespace sun
