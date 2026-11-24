#pragma once

#include <filesystem>
#include <string>

#include "abi/abi_compatibility_checker.h"

/** Reuses successful checks of immutable Moon contents across compilations. */
namespace sun::abi {
/** Selects a cache directory and force policy without leaking into parent
 * builds. */
class ValidationCacheScope {
 public:
  /** Inherits an enclosing directory when root is empty; force is cumulative.
   */
  explicit ValidationCacheScope(std::filesystem::path root = {},
                                bool force = false);
  /** Restores the enclosing cache policy. */
  ~ValidationCacheScope();
  /** Keeps policy lifetime tied to the original stack frame. */
  ValidationCacheScope(const ValidationCacheScope&) = delete;
  /** Prevents replacing an active policy. */
  ValidationCacheScope& operator=(const ValidationCacheScope&) = delete;

 private:
  std::filesystem::path previousRoot_;
  bool previousForce_;
};

/** Stores only successful bundle-local checks; import authorization stays
 * local. */
class BundleValidationCache {
 public:
  /** Returns the absolute directory, honoring the explicit environment
   * override. */
  static std::filesystem::path directory();
  /** Frames the exact bytes, implementation, runtime and compilation settings.
   */
  static std::string key(const std::string& digest,
                         const CompilationContext& context,
                         const std::string& target);
  /** Checks memory and disk receipts, treating I/O and format failures as
   * misses. */
  static bool contains(const std::string& key);
  /** Publishes a success receipt atomically; failure only loses an
   * optimization. */
  static void remember(const std::string& key);
  /** Discards in-process receipts, allowing independent-process behavior in
   * tests. */
  static void clearMemory();
};
}  // namespace sun::abi
