#include "driver/git_source.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "moon_bundling/moon.h"
#include "support/error.h"

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {
/** Keeps checkout and process helpers private to this file. */
namespace {
/** Runs Git with separate arguments, disabled hooks, and restricted protocols.
 */
void runGit(const std::vector<std::string>& arguments) {
  auto program = llvm::sys::findProgramByName("git");
  if (!program)
    sun::support::logAndThrowError(
        "Git source entrypoints require git on PATH");
  std::vector<std::string> storage = {*program,
                                      "-c",
                                      "core.hooksPath=/dev/null",
                                      "-c",
                                      "protocol.allow=never",
                                      "-c",
                                      "protocol.https.allow=always",
                                      "-c",
                                      "protocol.http.allow=always",
                                      "-c",
                                      "protocol.ssh.allow=always",
                                      "-c",
                                      "protocol.file.allow=always"};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<llvm::StringRef> args(storage.begin(), storage.end());
  std::string error;
  if (llvm::sys::ExecuteAndWait(*program, args, std::nullopt, {}, 0, 0,
                                &error) != 0) {
    sun::support::logAndThrowError("could not prepare Git source checkout" +
                                   (error.empty() ? "" : ": " + error));
  }
}

/** Selects the source cache without sharing compiled outputs across targets. */
std::filesystem::path cacheDirectory() {
  if (const char* cache = std::getenv("SUN_GIT_CACHE")) {
    if (*cache) return std::filesystem::absolute(cache);
  }
  if (const char* home = std::getenv("HOME"))
    return std::filesystem::path(home) / ".sun/cache/git";
  sun::support::logAndThrowError(
      "set SUN_GIT_CACHE or HOME to cache Git sources");
}
}  // namespace

void validateGitSource(const ConfigEntrypoint& entry) {
  const auto& url = entry.git;
  const auto colon = url.find(':');
  const bool scheme =
      url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0 ||
      url.rfind("ssh://", 0) == 0 || url.rfind("file://", 0) == 0;
  const bool scp = colon != std::string::npos && colon > 0 &&
                   colon + 1 < url.size() &&
                   url.find("://") == std::string::npos &&
                   url.substr(0, colon).find('/') == std::string::npos;
  if (url.empty() || url.front() == '-' || (!scheme && !scp) ||
      std::any_of(url.begin(), url.end(), [](unsigned char c) {
        return std::isspace(c) || std::iscntrl(c);
      })) {
    sun::support::logAndThrowError(
        "entrypoint 'git' must be an HTTP(S), SSH, file URL, or host:path SSH "
        "address");
  }
  if (entry.version.empty() || entry.version.front() == '-' ||
      entry.version.find_first_of(" ~^:?*[\\") != std::string::npos ||
      entry.version.find("..") != std::string::npos ||
      entry.version.find("@{") != std::string::npos ||
      std::any_of(entry.version.begin(), entry.version.end(),
                  [](unsigned char c) { return std::iscntrl(c); })) {
    sun::support::logAndThrowError(
        "Git entrypoint 'version' must be a commit ID, branch, or tag name");
  }
  if (entry.type != ConfigEntrypoint::Type::Library)
    sun::support::logAndThrowError("Git entrypoints must have type 'library'");
  const std::filesystem::path path(entry.path);
  if (path.empty() || path.is_absolute() ||
      std::any_of(path.begin(), path.end(),
                  [](const auto& part) { return part == ".."; })) {
    sun::support::logAndThrowError(
        "Git entrypoint 'path' must be relative and stay inside the "
        "repository");
  }
}

std::string resolveGitEntrypoint(const ConfigEntrypoint& entry, bool refresh) {
  namespace fs = std::filesystem;
  validateGitSource(entry);
  const auto cache = cacheDirectory() / sun::moon_bundling::computeSha256Hex(
                                            entry.git + "\n" + entry.version);
  fs::path checkout;
  std::string snapshot;
  std::ifstream(cache / "current") >> snapshot;
  if (!snapshot.empty() && fs::path(snapshot).filename() == snapshot &&
      snapshot != "." && snapshot != "..") {
    checkout = cache / snapshot;
  }
  if (refresh || checkout.empty() ||
      !fs::is_directory(checkout / ".sun-git-source")) {
    fs::create_directories(cache);
    llvm::SmallString<256> temporary;
    if (auto error = llvm::sys::fs::createUniqueDirectory(
            (cache / "checkout").string(), temporary)) {
      sun::support::logAndThrowError("could not create Git cache: " +
                                     error.message());
    }
    const fs::path staging(temporary.str().str());
    try {
      runGit({"init", "--quiet", "--template=", staging.string()});
      runGit({"-C", staging.string(), "fetch", "--quiet", "--no-tags",
              "--depth=1", "--", entry.git, entry.version});
      runGit({"-C", staging.string(), "checkout", "--quiet", "--detach",
              "FETCH_HEAD"});
      // Replace any repository-provided marker rather than following a symlink.
      fs::remove_all(staging / ".sun-git-source");
      fs::create_directory(staging / ".sun-git-source");
      const auto pointer = staging / ".sun-git-source/current";
      std::ofstream ready(pointer);
      ready << staging.filename().string() << '\n';
      ready.close();
      if (!ready)
        sun::support::logAndThrowError("could not write Git cache marker");
      // Publish only complete snapshots; concurrent readers keep their old
      // paths.
      fs::rename(pointer, cache / "current");
      checkout = staging;
    } catch (...) {
      std::error_code ignored;
      fs::remove_all(staging, ignored);
      throw;
    }
  }
  const auto source = fs::weakly_canonical(checkout / entry.path);
  const auto relative = source.lexically_relative(fs::canonical(checkout));
  if (relative.empty() || *relative.begin() == ".." ||
      !fs::is_regular_file(source))
    sun::support::logAndThrowError(
        "Git entrypoint is missing or escapes its checkout: " + entry.path);
  return source.string();
}
}  // namespace sun::driver
