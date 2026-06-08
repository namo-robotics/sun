// ast_cache.cpp — Implementation of AST caching system

#include "ast_cache.h"

#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <fstream>

#include "ast_deserializer.h"
#include "ast_serializer.h"

namespace sun {

// Default configuration
ASTCacheConfig ASTCacheConfig::defaultConfig() {
  ASTCacheConfig config;
  config.maxMemoryEntries = 100;
  config.maxDiskCacheMB = 500;
  config.enableDiskCache = true;
  config.enableAnalysisCache = true;

  // Default disk cache path: ~/.sun/cache/
  if (const char* home = std::getenv("HOME")) {
    config.diskCachePath = std::filesystem::path(home) / ".sun" / "cache";
  } else {
    config.diskCachePath = "/tmp/sun-cache";
  }

  return config;
}

ASTCache::ASTCache(ASTCacheConfig config) : config_(std::move(config)) {
  // Create disk cache directory if enabled
  if (config_.enableDiskCache && !config_.diskCachePath.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(config_.diskCachePath, ec);
    // Ignore errors - disk cache will just be disabled if creation fails
  }
}

ASTCache::~ASTCache() = default;

std::string ASTCache::computeHash(const std::string& content) {
  llvm::SHA256 sha;
  sha.update(llvm::StringRef(content));
  auto hashBytes = sha.final();

  std::string hash;
  hash.reserve(64);
  for (uint8_t b : hashBytes) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", b);
    hash += hex;
  }
  return hash;
}

std::string ASTCache::computeFileHash(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return computeHash(buffer.str());
}

std::filesystem::path ASTCache::getCachePath(
    const std::string& contentHash) const {
  // Use first 2 chars as subdirectory for better filesystem performance
  std::string subdir = contentHash.substr(0, 2);
  return config_.diskCachePath / subdir / (contentHash + ".ast.pb");
}

std::unique_ptr<BlockExprAST> ASTCache::get(const std::string& contentHash,
                                            bool requireAnalysis) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check memory cache first
  auto it = memoryCache_.find(contentHash);
  if (it != memoryCache_.end()) {
    CacheEntry& entry = it->second;

    // Check analysis requirement
    if (requireAnalysis && !entry.hasAnalysis) {
      // Entry exists but doesn't have analysis data
      stats_.misses++;
      return nullptr;
    }

    // Update access statistics
    entry.lastAccess = std::chrono::steady_clock::now();
    entry.accessCount++;
    stats_.memoryHits++;

    // Deserialize and return
    serialization::DeserializerConfig deserConfig;
    deserConfig.restore_analysis = entry.hasAnalysis;
    serialization::ASTDeserializer deserializer(deserConfig);
    stats_.deserializations++;
    return deserializer.deserializeProgramFromString(entry.serializedAst);
  }

  // Check disk cache
  if (config_.enableDiskCache) {
    auto diskData = loadFromDisk(contentHash);
    if (diskData) {
      // Promote to memory cache
      CacheEntry entry;
      entry.contentHash = contentHash;
      entry.serializedAst = *diskData;
      // Check if this is an analyzed AST (filename ends with .analyzed.ast.pb)
      entry.hasAnalysis = false;  // Will be set based on metadata in future
      entry.lastAccess = std::chrono::steady_clock::now();
      entry.accessCount = 1;

      evictIfNeeded();
      memoryCache_[contentHash] = std::move(entry);

      stats_.diskHits++;
      stats_.deserializations++;

      serialization::DeserializerConfig deserConfig;
      serialization::ASTDeserializer deserializer(deserConfig);
      return deserializer.deserializeProgramFromString(*diskData);
    }
  }

  stats_.misses++;
  return nullptr;
}

void ASTCache::put(const std::string& contentHash, const BlockExprAST& ast,
                   bool hasAnalysis) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Serialize the AST
  serialization::SerializerConfig serConfig;
  serConfig.include_analysis = hasAnalysis && config_.enableAnalysisCache;
  serConfig.include_location = true;
  serialization::ASTSerializer serializer(serConfig);

  std::string serialized = serializer.serializeProgramToString(ast);
  stats_.serializations++;

  // Evict if needed before adding
  evictIfNeeded();

  // Add to memory cache
  CacheEntry entry;
  entry.contentHash = contentHash;
  entry.serializedAst = std::move(serialized);
  entry.hasAnalysis = hasAnalysis;
  entry.lastAccess = std::chrono::steady_clock::now();
  entry.accessCount = 1;

  memoryCache_[contentHash] = entry;

  // Also persist to disk if enabled
  if (config_.enableDiskCache) {
    saveToDisk(contentHash, entry.serializedAst, hasAnalysis);
  }
}

bool ASTCache::contains(const std::string& contentHash,
                        bool requireAnalysis) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = memoryCache_.find(contentHash);
  if (it != memoryCache_.end()) {
    if (requireAnalysis && !it->second.hasAnalysis) {
      return false;
    }
    return true;
  }

  // Check disk cache
  if (config_.enableDiskCache) {
    auto path = getCachePath(contentHash);
    return std::filesystem::exists(path);
  }

  return false;
}

void ASTCache::invalidate(const std::string& contentHash) {
  std::lock_guard<std::mutex> lock(mutex_);

  memoryCache_.erase(contentHash);

  // Also remove from disk
  if (config_.enableDiskCache) {
    auto path = getCachePath(contentHash);
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

void ASTCache::invalidateByPath(const std::filesystem::path& filePath) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string pathStr = filePath.string();
  auto it = pathToHash_.find(pathStr);
  if (it != pathToHash_.end()) {
    std::string hash = it->second;
    memoryCache_.erase(hash);
    pathToHash_.erase(it);

    if (config_.enableDiskCache) {
      auto path = getCachePath(hash);
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }
}

void ASTCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);

  memoryCache_.clear();
  pathToHash_.clear();

  // Optionally clear disk cache
  if (config_.enableDiskCache && !config_.diskCachePath.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(config_.diskCachePath, ec);
    std::filesystem::create_directories(config_.diskCachePath, ec);
  }
}

CacheStats ASTCache::getStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void ASTCache::resetStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_ = CacheStats{};
}

std::optional<std::string> ASTCache::loadFromDisk(
    const std::string& contentHash) const {
  auto path = getCachePath(contentHash);
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void ASTCache::saveToDisk(const std::string& contentHash,
                          const std::string& data, bool hasAnalysis) {
  auto path = getCachePath(contentHash);

  // Create subdirectory if needed
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return;  // Silently fail disk cache write
  }

  std::ofstream file(path, std::ios::binary);
  if (file) {
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
}

void ASTCache::evictIfNeeded() {
  if (memoryCache_.size() < config_.maxMemoryEntries) {
    return;
  }

  // LRU eviction: find least recently accessed entry
  auto oldest = memoryCache_.begin();
  for (auto it = memoryCache_.begin(); it != memoryCache_.end(); ++it) {
    if (it->second.lastAccess < oldest->second.lastAccess) {
      oldest = it;
    }
  }

  if (oldest != memoryCache_.end()) {
    memoryCache_.erase(oldest);
    stats_.evictions++;
  }
}

void ASTCache::preload(const std::vector<std::filesystem::path>& files) {
  for (const auto& file : files) {
    std::string hash = computeFileHash(file);
    if (!hash.empty() && !contains(hash)) {
      // Load from disk cache if available
      if (config_.enableDiskCache) {
        auto diskData = loadFromDisk(hash);
        if (diskData) {
          std::lock_guard<std::mutex> lock(mutex_);
          CacheEntry entry;
          entry.contentHash = hash;
          entry.serializedAst = *diskData;
          entry.hasAnalysis = false;
          entry.lastAccess = std::chrono::steady_clock::now();
          entry.accessCount = 0;
          memoryCache_[hash] = std::move(entry);
          pathToHash_[file.string()] = hash;
        }
      }
    }
  }
}

// Singleton instance
ASTCache& ASTCache::instance() {
  static ASTCache globalCache;
  return globalCache;
}

}  // namespace sun
