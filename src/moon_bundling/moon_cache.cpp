// moon_cache.cpp — see moon_cache.h

#include "moon_bundling/moon_cache.h"

#include <llvm/Support/SHA256.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "support/error.h"

namespace sun {

std::filesystem::path MoonCache::defaultCacheDir() {
  if (const char* override = std::getenv("SUN_MOON_CACHE")) {
    return override;
  }
  if (const char* home = std::getenv("HOME")) {
    return std::filesystem::path(home) / ".sun" / "cache" / "moons";
  }
  return "/tmp/sun-cache/moons";
}

std::string MoonCache::computeSha256Hex(const std::string& content) {
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

std::string MoonCache::computeFileSha256Hex(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return computeSha256Hex(buffer.str());
}

// The cache filename never affects module names (those come from bundle
// metadata); the basename suffix is only for debuggability.
std::filesystem::path MoonCache::getCachePathFor(
    const std::string& url, const std::filesystem::path& cacheDir) {
  std::string basename;
  auto lastSlash = url.find_last_of('/');
  if (lastSlash != std::string::npos) {
    for (char c : url.substr(lastSlash + 1)) {
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' ||
          c == '-') {
        basename += c;
      }
    }
  }
  if (basename.empty()) {
    basename = "lib.moon";
  }
  return cacheDir / (computeSha256Hex(url) + "-" + basename);
}

// The URL is later single-quoted into a shell command, so instead of
// escaping, reject anything a quoted URL should never contain.
void MoonCache::validateUrl(const std::string& url) {
  bool schemeOk = url.rfind("http://", 0) == 0 ||
                  url.rfind("https://", 0) == 0 || url.rfind("file://", 0) == 0;
  if (!schemeOk) {
    logAndThrowError("moon url must start with http://, https:// or file://: " +
                     url);
  }
  for (char c : url) {
    if (c == '\'' || c == '\\' || std::isspace(static_cast<unsigned char>(c)) ||
        std::iscntrl(static_cast<unsigned char>(c))) {
      logAndThrowError("moon url contains unsupported characters: " + url);
    }
  }
}

namespace {
std::string githubToken_;
}  // namespace

void MoonCache::setGithubToken(const std::string& token) {
  githubToken_ = token;
}

// Explicit --gh-token wins; the gh CLI's environment variables are the
// fallback so the LSP (which takes no compiler flags) can authenticate too.
std::string MoonCache::getGithubToken() {
  if (!githubToken_.empty()) {
    return githubToken_;
  }
  if (const char* env = std::getenv("GH_TOKEN")) {
    return env;
  }
  if (const char* env = std::getenv("GITHUB_TOKEN")) {
    return env;
  }
  return "";
}

bool MoonCache::hasGithubHost(const std::string& url) {
  auto schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) {
    return false;
  }
  auto hostStart = schemeEnd + 3;
  auto hostEnd = url.find('/', hostStart);
  std::string host =
      url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos
                                                         : hostEnd - hostStart);
  auto endsWith = [&](const std::string& suffix) {
    return host.size() >= suffix.size() &&
           host.compare(host.size() - suffix.size(), suffix.size(), suffix) ==
               0;
  };
  return host == "github.com" || endsWith(".github.com") ||
         endsWith(".githubusercontent.com");
}

std::string MoonCache::buildCurlCommand(const std::string& url,
                                        const std::filesystem::path& dest) {
  std::string command = "curl -fsSL --connect-timeout 30 --max-time 300";

  // The token goes only to GitHub hosts, never to arbitrary servers.
  std::string token = getGithubToken();
  if (!token.empty() && hasGithubHost(url)) {
    for (char c : token) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' &&
          c != '-' && c != '.') {
        logAndThrowError("GitHub token contains unsupported characters");
      }
    }
    command += " -H 'Authorization: Bearer " + token + "'";
    // The GitHub API returns release-asset bytes only for this Accept type
    if (url.rfind("https://api.github.com/", 0) == 0) {
      command += " -H 'Accept: application/octet-stream'";
    }
  }

  command += " --output '" + dest.string() + "' '" + url + "'";
  return command;
}

void MoonCache::downloadWithCurl(const std::string& url,
                                 const std::filesystem::path& dest) {
  std::string command = buildCurlCommand(url, dest);
  int status = std::system(command.c_str());
  if (status == 0) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove(dest, ec);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
    logAndThrowError("downloading moon requires 'curl' on PATH");
  }
  int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : status;
  logAndThrowError("failed to download moon '" + url + "' (curl exit " +
                   std::to_string(exitCode) + ")");
}

std::filesystem::path MoonCache::fetch(
    const std::string& url, const std::optional<std::string>& expectedHash,
    const std::filesystem::path& cacheDir) {
  validateUrl(url);

  std::string wantedHash;
  if (expectedHash) {
    wantedHash = *expectedHash;
    std::transform(wantedHash.begin(), wantedHash.end(), wantedHash.begin(),
                   [](unsigned char c) { return std::tolower(c); });
  }

  std::error_code ec;
  std::filesystem::create_directories(cacheDir, ec);

  auto dest = getCachePathFor(url, cacheDir);
  if (std::filesystem::exists(dest)) {
    if (wantedHash.empty() || computeFileSha256Hex(dest) == wantedHash) {
      return std::filesystem::absolute(dest);
    }
    // Cached file no longer matches the manifest hash: fetch again.
  }

  auto tmp = dest;
  tmp += ".tmp." + std::to_string(getpid());
  downloadWithCurl(url, tmp);

  if (!wantedHash.empty()) {
    auto actual = computeFileSha256Hex(tmp);
    if (actual != wantedHash) {
      std::filesystem::remove(tmp, ec);
      logAndThrowError("hash mismatch for moon '" + url +
                       "': manifest expects " + wantedHash +
                       ", downloaded file is " + actual);
    }
  }

  // Atomic within the cache directory, so an interrupted or concurrent
  // compile never leaves a partial bundle at the final path.
  std::filesystem::rename(tmp, dest, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    logAndThrowError("failed to store downloaded moon '" + url + "' in " +
                     cacheDir.string());
  }
  return std::filesystem::absolute(dest);
}

}  // namespace sun
