#pragma once

#include <filesystem>
#include <optional>
#include <string>

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {

/**
 * Download cache for .moon bundles referenced by URL in a manifest:
 *
 *   manifest { libraries: [{ url: "https://example.com/lib.moon" }] }
 *
 * Bundles are downloaded once with the system curl binary and stored under
 * the cache directory, keyed by the URL. A cached bundle is reused forever,
 * unless the manifest entry carries a `hash` (lowercase hex SHA-256 of the
 * file bytes) that the cached file no longer matches — then it is fetched
 * again and the fresh download must match the hash.
 */
class MoonCache {
 public:
  /**
   * ~/.sun/cache/moons, or $SUN_MOON_CACHE when set; /tmp/sun-cache/moons
   * when HOME is unset.
   */
  static std::filesystem::path defaultCacheDir();

  /**
   * Absolute path of the cached bundle for `url`, downloading on a cache
   * miss or hash mismatch. Throws SunError on a malformed URL, a failed
   * download, or a hash mismatch on the fresh download.
   */
  static std::filesystem::path fetch(
      const std::string& url, const std::optional<std::string>& expectedHash,
      const std::filesystem::path& cacheDir = defaultCacheDir());

  /**
   * Token for downloading private GitHub assets (--gh-token). Sent as an
   * Authorization header, and only to GitHub hosts. When unset, the GH_TOKEN
   * and GITHUB_TOKEN environment variables are used, in that order.
   */
  static void setGithubToken(const std::string& token);

  /**
   * The curl invocation fetch would run for `url`. Exposed for tests.
   */
  static std::string buildCurlCommand(const std::string& url,
                                      const std::filesystem::path& dest);

 private:
  /** Returns the hexadecimal SHA-256 digest of the supplied contents. */
  static std::string computeSha256Hex(const std::string& content);
  /** Reads a file and returns its hexadecimal SHA-256 digest. */
  static std::string computeFileSha256Hex(const std::filesystem::path& path);
  /** Derives the cache filename for the supplied library URL. */
  static std::filesystem::path getCachePathFor(
      const std::string& url, const std::filesystem::path& cacheDir);
  /** Checks whether a library download URL uses an accepted form. */
  static void validateUrl(const std::string& url);
  /** Returns the GitHub authentication token. */
  static std::string getGithubToken();
  /** Reports whether this object has github host. */
  static bool hasGithubHost(const std::string& url);
  /** Downloads a library to the destination path using curl. */
  static void downloadWithCurl(const std::string& url,
                               const std::filesystem::path& dest);
};

}  // namespace sun::moon_bundling
