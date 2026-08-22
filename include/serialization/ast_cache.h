// ast_cache.h — Cache for parsed and analyzed ASTs
//
// Provides multi-level caching for ASTs:
// 1. Memory cache: Fast in-process lookup by content hash
// 2. Disk cache: Persistent storage for analyzed ASTs (imports, stdlib)
//
// Used by LSP for fast diagnostics and by compiler for import caching.

#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "ast.h"

namespace sun {

/// Statistics for cache performance monitoring
struct CacheStats {
  size_t memoryHits = 0;
  size_t diskHits = 0;
  size_t misses = 0;
  size_t serializations = 0;
  size_t deserializations = 0;
  size_t evictions = 0;

  double hitRate() const {
    size_t total = memoryHits + diskHits + misses;
    return total > 0 ? static_cast<double>(memoryHits + diskHits) / total : 0.0;
  }
};

/// Entry in the memory cache
struct CacheEntry {
  std::string contentHash;    // SHA-256 of source content
  std::string serializedAst;  // Protobuf-serialized AST
  bool hasAnalysis = false;   // Whether semantic analysis is included
  std::chrono::steady_clock::time_point lastAccess;
  size_t accessCount = 0;
};

/// Configuration for the AST cache
struct ASTCacheConfig {
  size_t maxMemoryEntries = 100;        // Max entries in memory cache
  size_t maxDiskCacheMB = 500;          // Max disk cache size in MB
  bool enableDiskCache = true;          // Whether to use disk persistence
  bool enableAnalysisCache = true;      // Cache post-semantic-analysis ASTs
  std::filesystem::path diskCachePath;  // Where to store disk cache

  static ASTCacheConfig defaultConfig();
};

/// Thread-safe AST cache with memory and disk layers
class ASTCache {
 public:
  explicit ASTCache(ASTCacheConfig config = ASTCacheConfig::defaultConfig());
  ~ASTCache();

  // Disable copy
  ASTCache(const ASTCache&) = delete;
  ASTCache& operator=(const ASTCache&) = delete;

  /// Get a cached AST, or nullptr if not found
  /// @param contentHash Hash of the source content
  /// @param requireAnalysis If true, only return if analysis data is present
  std::unique_ptr<BlockExprAST> get(const std::string& contentHash,
                                    bool requireAnalysis = false);

  /// Store an AST in the cache
  /// @param contentHash Hash of the source content
  /// @param ast The AST to cache
  /// @param hasAnalysis Whether the AST has semantic analysis data
  void put(const std::string& contentHash, const BlockExprAST& ast,
           bool hasAnalysis = false);

  /// Check if a hash exists in cache (memory or disk)
  bool contains(const std::string& contentHash,
                bool requireAnalysis = false) const;

  /// Invalidate a specific cache entry
  void invalidate(const std::string& contentHash);

  /// Invalidate all entries for a file path
  void invalidateByPath(const std::filesystem::path& filePath);

  /// Clear all cached data
  void clear();

  /// Get cache statistics
  CacheStats getStats() const;

  /// Reset statistics
  void resetStats();

  /// Compute content hash for source text
  static std::string computeHash(const std::string& content);

  /// Compute content hash for a file
  static std::string computeFileHash(const std::filesystem::path& path);

  /// Preload frequently-used ASTs (e.g., stdlib) into memory cache
  void preload(const std::vector<std::filesystem::path>& files);

  /// Get the singleton instance (for global caching)
  static ASTCache& instance();

 private:
  ASTCacheConfig config_;
  mutable std::mutex mutex_;
  mutable CacheStats stats_;

  // Memory cache: hash -> serialized AST
  std::unordered_map<std::string, CacheEntry> memoryCache_;

  // Path to hash mapping for invalidation
  std::unordered_map<std::string, std::string> pathToHash_;

  // Internal helpers
  std::optional<std::string> loadFromDisk(const std::string& contentHash) const;
  void saveToDisk(const std::string& contentHash, const std::string& data,
                  bool hasAnalysis);
  void evictIfNeeded();
  std::filesystem::path getCachePath(const std::string& contentHash) const;
};

}  // namespace sun
