// command_support.cpp — small helpers that more than one `sun` command needs.

#include "cli/command_support.h"

#include <filesystem>
#include <iostream>
#include <set>

#include "driver/git_source.h"
#include "driver/manifest_processor.h"
#include "llvm/Support/raw_ostream.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/moon_cache.h"
#include "support/sun_path.h"

using sun::driver::SunConfig;
using sun::moon_bundling::LibraryCache;

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

DependencyPathVariables::DependencyPathVariables(
    const SunConfig& config, const sun::driver::ConfigEntrypoint& entry)
    : previous_(sun::driver::ManifestProcessor::exchangeDependencyPathVariables(
          entry.git.empty() ? std::map<std::string, std::string>{}
                            : config.pathVariables)) {}

DependencyPathVariables::~DependencyPathVariables() {
  sun::driver::ManifestProcessor::exchangeDependencyPathVariables(
      std::move(previous_));
}

/** Reports whether the input names a project configuration file. */
bool isConfigInput(const std::string& input) {
  return std::filesystem::path(input).filename() == SunConfig::kFileName;
}

/** Loads project configuration for the selected compilation target. */
SunConfig loadConfigInput(const std::string& input,
                          const std::string& targetTriple, bool refreshSources) {
  SunConfig config =
      SunConfig::loadFile(std::filesystem::absolute(input), targetTriple);
  if (config.entrypoints.empty()) {
    sun::support::logAndThrowError(
        input +
        " declares no entrypoints; add an 'entrypoints' list or "
        "name a .sun file directly");
  }
  // Cached sources live outside the project, so carry its selected settings.
  for (const auto& path : config.sunPath) {
    sun::support::SunPath::addSearchPath(path);
    LibraryCache::instance().addSearchPath(path);
  }
  for (const auto& [name, value] : config.pathVariables)
    sun::driver::ManifestProcessor::setDefaultPathVariable(name, value);
  std::set<std::pair<std::string, std::string>> refreshed;
  for (auto& entry : config.entrypoints) {
    if (!entry.git.empty()) {
      const bool refresh = refreshSources &&
                           refreshed.emplace(entry.git, entry.version).second;
      entry.path = sun::driver::resolveGitEntrypoint(entry, refresh);
    }
  }
  return config;
}

/** Applies command-line settings shared by compiler commands. */
void applySharedSettings(const SharedOptions& shared) {
  for (const auto& [name, value] : shared.pathVariables) {
    sun::driver::ManifestProcessor::setPathVariable(name, value);
  }
  LibraryCache::instance().initFromEnvironment();
  for (const auto& libPath : shared.libPaths) {
    LibraryCache::instance().addSearchPath(libPath);
    // Manifest `libraries:` entries resolve through SunPath, so --lib-path
    // has to reach it too, not just the bundle cache.
    sun::support::SunPath::addSearchPath(libPath);
  }
}

/** Applies the build and execution settings selected on the command line. */
void applyBuildRunSettings(const BuildRunOptions& options) {
  if (!options.githubToken.empty()) {
    sun::moon_bundling::MoonCache::setGithubToken(options.githubToken);
  }
  // The target has to be known before the search paths are read
  LibraryCache::instance().setTargetTriple(options.targetTriple);
  applySharedSettings(options.shared);
}

/** Prints an early-exit message to its selected output stream. */
int reportEarlyExit(const EarlyExit& earlyExit) {
  if (earlyExit.stream == EarlyExit::Stream::Out) {
    llvm::outs() << earlyExit.text;
  } else {
    llvm::errs() << earlyExit.text;
  }
  return earlyExit.exitCode;
}

/** Prints a compiler diagnostic for the command-line user. */
int reportSunError(const sun::support::SunError& error) {
  std::cerr << error.what() << std::endl;
  return 1;
}

/** Prints an unexpected exception as a command-line failure. */
int reportUnexpectedError(const std::exception& error) {
  std::cerr << "Error: " << error.what() << std::endl;
  return 1;
}

/** Recognizes the diagnostic produced when no tests are available. */
bool isNoTestsError(const sun::support::SunError& error) {
  return std::string(error.what()).find("no test functions found") !=
         std::string::npos;
}

}  // namespace sun::cli
