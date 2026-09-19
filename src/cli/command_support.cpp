// command_support.cpp — small helpers that more than one `sun` command needs.

#include "cli/command_support.h"

#include <filesystem>
#include <iostream>

#include "driver/manifest_processor.h"
#include "llvm/Support/raw_ostream.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/moon_cache.h"
#include "support/sun_path.h"

using sun::driver::SunConfig;
using sun::moon_bundling::LibraryCache;

namespace sun::cli {

bool isConfigInput(const std::string& input) {
  return std::filesystem::path(input).filename() == SunConfig::kFileName;
}

SunConfig loadConfigInput(const std::string& input,
                          const std::string& targetTriple) {
  SunConfig config =
      SunConfig::loadFile(std::filesystem::absolute(input), targetTriple);
  if (config.entrypoints.empty()) {
    sun::support::logAndThrowError(
        input +
        " declares no entrypoints; add an 'entrypoints' list or "
        "name a .sun file directly");
  }
  return config;
}

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

void applyBuildRunSettings(const BuildRunOptions& options) {
  if (!options.githubToken.empty()) {
    sun::moon_bundling::MoonCache::setGithubToken(options.githubToken);
  }
  // The target has to be known before the search paths are read
  LibraryCache::instance().setTargetTriple(options.targetTriple);
  applySharedSettings(options.shared);
}

int reportEarlyExit(const EarlyExit& earlyExit) {
  if (earlyExit.stream == EarlyExit::Stream::Out) {
    llvm::outs() << earlyExit.text;
  } else {
    llvm::errs() << earlyExit.text;
  }
  return earlyExit.exitCode;
}

int reportSunError(const sun::support::SunError& error) {
  std::cerr << error.what() << std::endl;
  return 1;
}

int reportUnexpectedError(const std::exception& error) {
  std::cerr << "Error: " << error.what() << std::endl;
  return 1;
}

bool isNoTestsError(const sun::support::SunError& error) {
  return std::string(error.what()).find("no test functions found") !=
         std::string::npos;
}

}  // namespace sun::cli
