#include "library_cache.h"

#include <algorithm>
#include <cstdlib>

namespace sun {

LibraryCache& LibraryCache::instance() {
  static LibraryCache cache;
  return cache;
}

void LibraryCache::addSearchPath(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Avoid duplicates
  if (std::find(searchPaths_.begin(), searchPaths_.end(), path) ==
      searchPaths_.end()) {
    searchPaths_.push_back(path);
    discovered_ = false;  // Need to re-discover
  }
}

void LibraryCache::addBundle(const std::filesystem::path& bundlePath) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if already loaded
  for (const auto& bundle : bundles_) {
    if (bundle->getPath() == bundlePath) {
      return;
    }
  }

  auto reader = SunLibReader::open(bundlePath);
  if (reader) {
    // Index all modules in this bundle
    for (const auto& modPath : reader->listModules()) {
      moduleToBundle_[modPath] = reader.get();
    }
    bundles_.push_back(std::move(reader));
  }
}

void LibraryCache::initFromEnvironment() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (initialized_) return;

  const char* sunPath = std::getenv("SUN_PATH");
  if (sunPath) {
    std::filesystem::path basePath(sunPath);

    // Add standard search paths
    auto libPath = basePath / "lib";
    auto buildPath = basePath / "build";

    if (std::filesystem::exists(libPath)) {
      searchPaths_.push_back(libPath);
    }
    if (std::filesystem::exists(buildPath)) {
      searchPaths_.push_back(buildPath);
    }
  }

  // Also check current directory
  if (std::filesystem::exists("lib")) {
    searchPaths_.push_back("lib");
  }
  if (std::filesystem::exists("build")) {
    searchPaths_.push_back("build");
  }

  // System-wide installation paths (Debian package)
  if (std::filesystem::exists("/usr/lib/sun")) {
    searchPaths_.push_back("/usr/lib/sun");
  }
  if (std::filesystem::exists("/usr/share/sun/stdlib")) {
    searchPaths_.push_back("/usr/share/sun/stdlib");
  }

  initialized_ = true;
}

void LibraryCache::discoverBundles() {
  // Must be called with lock held
  if (discovered_) return;

  for (const auto& searchPath : searchPaths_) {
    if (!std::filesystem::exists(searchPath)) continue;

    // Look for .moon files
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(searchPath)) {
      if (entry.is_regular_file() && entry.path().extension() == ".moon") {
        // Check if already loaded
        bool alreadyLoaded = false;
        for (const auto& bundle : bundles_) {
          if (bundle->getPath() == entry.path()) {
            alreadyLoaded = true;
            break;
          }
        }

        if (!alreadyLoaded) {
          auto reader = SunLibReader::open(entry.path());
          if (reader) {
            for (const auto& modPath : reader->listModules()) {
              moduleToBundle_[modPath] = reader.get();
            }
            bundles_.push_back(std::move(reader));
          }
        }
      }
    }
  }

  discovered_ = true;
}

SunLibReader* LibraryCache::findBundleForModule(const std::string& moduleKey) {
  // Check cache first
  auto it = moduleToBundle_.find(moduleKey);
  if (it != moduleToBundle_.end()) {
    return it->second;
  }

  // Ensure bundles are discovered
  discoverBundles();

  // Check again after discovery
  it = moduleToBundle_.find(moduleKey);
  if (it != moduleToBundle_.end()) {
    return it->second;
  }

  return nullptr;
}

bool LibraryCache::hasModule(const std::string& moduleKey) {
  std::lock_guard<std::mutex> lock(mutex_);
  return findBundleForModule(moduleKey) != nullptr;
}

const ModuleMetadata* LibraryCache::getMetadata(const std::string& moduleKey) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto* bundle = findBundleForModule(moduleKey);
  if (!bundle) {
    return nullptr;
  }

  return bundle->getMetadata(moduleKey);
}

std::unique_ptr<llvm::Module> LibraryCache::loadModule(
    const std::string& moduleKey, llvm::LLVMContext& context) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto* bundle = findBundleForModule(moduleKey);
  if (!bundle) {
    return nullptr;
  }

  return bundle->loadModule(moduleKey, context);
}

const std::vector<std::filesystem::path>& LibraryCache::getSearchPaths() const {
  return searchPaths_;
}

void LibraryCache::preloadAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  discoverBundles();

  // Preload all metadata
  for (auto& bundle : bundles_) {
    for (const auto& modPath : bundle->listModules()) {
      bundle->getMetadata(modPath);
    }
  }
}

void LibraryCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  bundles_.clear();
  moduleToBundle_.clear();
  searchPaths_.clear();
  initialized_ = false;
  discovered_ = false;
}

}  // namespace sun
