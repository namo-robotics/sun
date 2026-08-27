#include "moon_bundling/library_cache.h"

#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <fstream>

#include "support/sun_path.h"

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

  // Already loaded (possibly by directory discovery): just mark it as an
  // explicit registration so selection can prefer it among equals.
  for (const auto& bundle : bundles_) {
    if (bundle->getPath() == bundlePath) {
      pinnedBundles_.insert(bundle.get());
      return;
    }
  }

  auto reader = MoonReader::open(bundlePath);
  if (reader) {
    // Index all modules in this bundle
    for (const auto& modPath : reader->listModules()) {
      moduleToBundle_[modPath].push_back(reader.get());
    }
    pinnedBundles_.insert(reader.get());
    bundles_.push_back(std::move(reader));
  }
}

void LibraryCache::setTargetTriple(const std::string& triple) {
  std::lock_guard<std::mutex> lock(mutex_);
  targetTriple_ = triple;
}

MoonReader* LibraryCache::selectBundle(
    const std::vector<MoonReader*>& candidates) const {
  if (candidates.empty()) return nullptr;

  llvm::Triple want(targetTriple_.empty() ? llvm::sys::getDefaultTargetTriple()
                                          : targetTriple_);

  // Explicit registration outranks discovery: imports resolve bundles by
  // exact name, and the linker must land on the same file the parser
  // resolved — if that bundle is wrong for the target, the link-time triple
  // check reports it with an actionable error rather than silently
  // substituting a different file. Target match breaks ties (several
  // explicitly registered bundles can accumulate across compilations in one
  // process), with the operating system weighed alongside the architecture
  // so an aarch64 Linux bundle never outranks the macOS one on a Mac.
  MoonReader* best = nullptr;
  int bestScore = -1;
  for (auto* reader : candidates) {
    std::string tripleStr = reader->getTargetTriple();
    int score = pinnedBundles_.count(reader) ? 4 : 0;
    if (!tripleStr.empty()) {
      llvm::Triple triple(tripleStr);
      bool archMatch = triple.getArch() == want.getArch();
      if (archMatch) score += 1;
      if (archMatch && sameOsFamily(triple, want)) score += 1;
    }
    if (score > bestScore) {
      best = reader;
      bestScore = score;
    }
  }
  return best;
}

void LibraryCache::initFromEnvironment() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (initialized_) return;

  // Add lib/ and build/ subdirectories from each SUN_PATH entry
  for (const auto& path : SunPath::getLibrarySearchPaths()) {
    searchPaths_.push_back(path);
  }

  // Also check current directory
  if (std::filesystem::exists("lib")) {
    searchPaths_.push_back("lib");
  }
  if (std::filesystem::exists("build")) {
    searchPaths_.push_back("build");
  }

  // System-wide installation paths: next to the compiler binary, then the
  // Debian and Homebrew prefixes (see SunPath::systemInstallDirs).
  for (const auto& dir : SunPath::systemInstallDirs()) {
    searchPaths_.push_back(dir);
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
          auto reader = MoonReader::open(entry.path());
          if (reader) {
            for (const auto& modPath : reader->listModules()) {
              moduleToBundle_[modPath].push_back(reader.get());
            }
            bundles_.push_back(std::move(reader));
          }
        }
      }
    }
  }

  discovered_ = true;
}

MoonReader* LibraryCache::findBundleForModule(const std::string& moduleKey) {
  // Discover before selecting, not merely as a fallback: an explicitly
  // added bundle (e.g. the manifest's stdlib.moon) may have target-specific
  // siblings in the search paths that are better candidates.
  discoverBundles();

  auto it = moduleToBundle_.find(moduleKey);
  if (it != moduleToBundle_.end()) {
    return selectBundle(it->second);
  }

  return nullptr;
}

std::vector<std::string> LibraryCache::extractNativeArchives(
    const std::set<std::string>& moduleKeys,
    const std::filesystem::path& destDir) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> extracted;
  std::set<MoonReader*> seenBundles;

  for (const auto& moduleKey : moduleKeys) {
    auto* bundle = findBundleForModule(moduleKey);
    if (!bundle) continue;
    if (bundle->getNativeArchives().empty()) continue;
    if (!seenBundles.insert(bundle).second) continue;  // one visit per bundle

    std::error_code ec;
    std::filesystem::path bundleDir =
        destDir / bundle->getPath().stem().string();
    std::filesystem::create_directories(bundleDir, ec);
    if (ec) continue;

    for (const auto& entry : bundle->getNativeArchives()) {
      std::vector<char> data;
      if (!bundle->readNativeArchive(entry.name, data)) continue;
      std::filesystem::path out = bundleDir / entry.name;
      std::ofstream file(out, std::ios::binary);
      if (!file) continue;
      file.write(data.data(), static_cast<std::streamsize>(data.size()));
      if (!file.good()) continue;
      file.close();
      extracted.push_back(out.string());
    }
  }

  return extracted;
}

bool LibraryCache::hasModule(const std::string& moduleKey) {
  std::lock_guard<std::mutex> lock(mutex_);
  return findBundleForModule(moduleKey) != nullptr;
}

const moon::ModuleMetadata* LibraryCache::getMetadata(
    const std::string& moduleKey) {
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

std::string LibraryCache::getBitcodeId(const std::string& moduleKey) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto* bundle = findBundleForModule(moduleKey);
  if (!bundle) {
    return "";
  }

  return bundle->getBitcodeId(moduleKey);
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
  pinnedBundles_.clear();
  searchPaths_.clear();
  initialized_ = false;
  discovered_ = false;
}

}  // namespace sun
