// command_support.cpp — small helpers that more than one `sun` command needs.

#include "cli/command_support.h"

#include <filesystem>
#include <iostream>

#include "driver/manifest_processor.h"
#include "llvm/Support/raw_ostream.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/moon_cache.h"
#include "support/sun_path.h"

namespace sun::cli {

bool isConfigInput(const std::string& input) {
  return std::filesystem::path(input).filename() == sun::SunConfig::kFileName;
}

sun::SunConfig loadConfigInput(const std::string& input,
                               const std::string& targetTriple) {
  sun::SunConfig config =
      sun::SunConfig::loadFile(std::filesystem::absolute(input), targetTriple);
  if (config.entrypoints.empty()) {
    logAndThrowError(input +
                     " declares no entrypoints; add an 'entrypoints' list or "
                     "name a .sun file directly");
  }
  return config;
}

void applySharedSettings(const SharedOptions& shared) {
  for (const auto& [name, value] : shared.pathVariables) {
    sun::ManifestProcessor::setPathVariable(name, value);
  }
  sun::LibraryCache::instance().initFromEnvironment();
  for (const auto& libPath : shared.libPaths) {
    sun::LibraryCache::instance().addSearchPath(libPath);
    // Manifest `libraries:` entries resolve through SunPath, so --lib-path
    // has to reach it too, not just the bundle cache.
    sun::SunPath::addSearchPath(libPath);
  }
}

void applyBuildRunSettings(const BuildRunOptions& options) {
  if (!options.githubToken.empty()) {
    sun::MoonCache::setGithubToken(options.githubToken);
  }
  // The target has to be known before the search paths are read
  sun::LibraryCache::instance().setTargetTriple(options.targetTriple);
  applySharedSettings(options.shared);
}

int writeDepfileOnSuccess(int exitCode, const sun::Depfile& depfile,
                          const std::string& depfilePath) {
  if (exitCode != 0 || depfilePath.empty()) return exitCode;
  try {
    depfile.write(depfilePath);
  } catch (const SunError& e) {
    return reportSunError(e);
  }
  return 0;
}

int reportEarlyExit(const EarlyExit& earlyExit) {
  if (earlyExit.stream == EarlyExit::Stream::Out) {
    llvm::outs() << earlyExit.text;
  } else {
    llvm::errs() << earlyExit.text;
  }
  return earlyExit.exitCode;
}

int reportSunError(const SunError& error) {
  std::cerr << error.what() << std::endl;
  return 1;
}

int reportUnexpectedError(const std::exception& error) {
  std::cerr << "Error: " << error.what() << std::endl;
  return 1;
}

bool isNoTestsError(const SunError& error) {
  return std::string(error.what()).find("no test functions found") !=
         std::string::npos;
}

}  // namespace sun::cli
