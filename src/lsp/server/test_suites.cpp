/** Language server test suites implementation. */
#include <sys/stat.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "driver/build_graph.h"
#include "driver/build_record.h"
#include "driver/manifest_processor.h"
#include "lsp/declarations.h"
#include "server.h"
#include "support/sun_path.h"

using sun::driver::ManifestProcessor;

/** Implements the Sun language server. */
namespace sun::lsp {

/** Matches Node's bigint stat identity before launching a capability-checked
 * binary. */
static std::optional<std::string> testBinaryIdentity(const std::string& path) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return std::nullopt;
  }
#ifdef __APPLE__
  const auto modified = info.st_mtimespec;
  const auto changed = info.st_ctimespec;
#else
  const auto modified = info.st_mtim;
  const auto changed = info.st_ctim;
#endif
  return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) + ":" +
         std::to_string(info.st_size) + ":" +
         std::to_string(modified.tv_sec * 1000000000LL + modified.tv_nsec) +
         ":" + std::to_string(changed.tv_sec * 1000000000LL + changed.tv_nsec);
}

/** Retains missing included configs and glob patterns so their creation can
 * refresh discovery. */
static void collectTestConfigPaths(const std::string& file,
                                   std::set<std::string>& files,
                                   std::set<std::string>& patterns) {
  const auto absolute = normalizePath(file);
  if (!files.insert(absolute).second) {
    return;
  }
  try {
    const auto config = sun::driver::SunConfig::loadFile(absolute);
    for (const auto& project : config.projects) {
      if (project.find_first_of("*?[") != std::string::npos) {
        patterns.insert(project);
      } else {
        collectTestConfigPaths(project, files, patterns);
      }
    }
  } catch (const std::exception&) {
    // The graph loader reports the error; retain this path for recovery.
  }
}

/** Timestamp-based freshness and the reason shown in the editor. */
struct TestFreshness {
  std::string status = "fresh";
  std::string reason = "Inputs are not newer than the test binary";
};

/** Checks the same resolved inputs for test and production artifacts. */
static TestFreshness checkTestFreshness(const std::string& binary,
                                        const std::set<std::string>& inputs,
                                        std::string problem) {
  namespace fs = std::filesystem;
  TestFreshness freshness;
  std::error_code ec;
  const auto built = fs::last_write_time(binary, ec);
  const bool readableTime = !ec;
  if (!readableTime) {
    problem = "Cannot read binary timestamp: " + binary;
  }
  for (const auto& input : inputs) {
    const auto modified = fs::last_write_time(input, ec);
    if (ec || !fs::is_regular_file(input, ec)) {
      problem = "Cannot check input: " + input;
      continue;
    }
    if (readableTime && modified > built) {
      freshness = {"stale", "Input changed: " + input};
    }
    const auto buffer = openSourceOverrides.find(normalizePath(input));
    if (buffer != openSourceOverrides.end()) {
      std::ifstream file(input);
      std::stringstream text;
      text << file.rdbuf();
      if (!file || file.bad()) {
        problem = "Cannot read input: " + input;
      } else if (text.str() != buffer->second) {
        freshness = {"stale", "Unsaved changes: " + input};
      }
    }
  }
  if (!problem.empty() && freshness.status != "stale") {
    freshness = {"unknown", std::move(problem)};
  }
  return freshness;
}

/** Lists expected test artifacts and checks their inputs without semantic
 * analysis. */
llvm::json::Object testSuites() {
  namespace fs = std::filesystem;
  std::map<std::string, sun::driver::ConfigEntrypoint> entries;
  std::set<std::string> configs, configPatterns;
  for (const auto& root : configuredSunConfigs) {
    collectTestConfigPaths(root, configs, configPatterns);
  }
  llvm::json::Array errors;
  sun::driver::setConfiguredBuildRoots(configuredSunConfigs);
  for (const auto& root : configuredSunConfigs) {
    try {
      const auto graph = sun::driver::loadConfigGraph(root);
      for (const auto& file : graph.projects) {
        collectTestConfigPaths(file.string(), configs, configPatterns);
        const auto config = sun::driver::configureBuildPaths(
            graph, sun::driver::SunConfig::loadFile(file));
        for (auto entry : config.entrypoints) {
          if (!entry.git.empty()) {
            continue;
          }
          entry.path = normalizePath(
              ManifestProcessor::expandPathVariables(entry.path, &config));
          entry.testBinaryName = ManifestProcessor::expandPathVariables(
              entry.testBinaryName, &config);
          entry.outputName =
              ManifestProcessor::expandPathVariables(entry.outputName, &config);
          entries.emplace(entry.path, std::move(entry));
        }
      }
    } catch (const std::exception& error) {
      errors.push_back(root + ": " + error.what());
    }
  }
  for (const auto& file : explicitEntrypoints) {
    sun::driver::ConfigEntrypoint entry;
    entry.path = normalizePath(file);
    entries.emplace(entry.path, std::move(entry));
  }
  // Match the existing editor behavior for open manifest entrypoints.
  for (const auto& entry : entrypointManager.entrypoints()) {
    if (!openSourceOverrides.count(entry.entrypointPath)) {
      continue;
    }
    sun::driver::ConfigEntrypoint candidate;
    candidate.path = entry.entrypointPath;
    entries.emplace(candidate.path, std::move(candidate));
  }

  llvm::json::Array suites;
  std::set<std::string> watched = configs;
  std::set<std::string> binaries;
  for (const auto& [path, entry] : entries) {
    std::set<std::string> inputs = configs;
    inputs.insert(path);
    std::string problem =
        errors.empty() ? "" : "Cannot resolve project configuration";
    try {
      const auto manifest = ManifestProcessor::fromEntrypointFile(path);
      if (manifest) {
        inputs.insert(manifest->sunFiles.begin(), manifest->sunFiles.end());
        inputs.insert(manifest->testSunFiles.begin(),
                      manifest->testSunFiles.end());
        inputs.insert(manifest->protoFiles.begin(), manifest->protoFiles.end());
        for (const auto& moon : manifest->moonImports) {
          inputs.insert(moon.path);
        }
        for (const auto& archive : manifest->nativeArchives) {
          inputs.insert(archive.path);
        }
      } else {
        problem = "Cannot resolve entrypoint manifest: " + path;
      }
      // Ancestor configs can supply manifest variables even outside the graph.
      for (auto directory = fs::path(path).parent_path(); !directory.empty();) {
        const auto configFile = directory / "sun-config.json";
        if (fs::exists(configFile)) {
          inputs.insert(configFile.string());
          if (sun::driver::SunConfig::loadFile(configFile).root) {
            break;
          }
        }
        auto parent = directory.parent_path();
        if (parent == directory) {
          break;
        }
        directory = parent;
      }
    } catch (const std::exception& error) {
      problem = error.what();
    }
    const auto& binary = entry.testBinaryName;
    std::error_code ec;
    const bool exists = !binary.empty() && fs::is_regular_file(binary, ec);
    const auto identity = testBinaryIdentity(binary);
    const auto record =
        exists ? sun::driver::readBuildRecord(binary) : std::nullopt;
    const auto freshness =
        exists ? checkTestFreshness(binary, inputs, problem)
               : TestFreshness{"missing", binary.empty()
                                              ? "No test binary is configured"
                                              : "Build required"};
    llvm::json::Array sources;
    for (const auto& input : inputs) {
      const auto absolute = normalizePath(input);
      watched.insert(absolute);
      sources.push_back(absolute);
    }
    // Keep watching artifacts even while metadata hides a target with no tests.
    for (const auto& artifact : {binary, entry.outputName}) {
      if (!artifact.empty()) {
        const auto absolute = normalizePath(artifact);
        watched.insert(absolute);
        binaries.insert(absolute);
      }
    }
    // Fresh production metadata can prove that this target has no tests.
    if (!exists && problem.empty() && !entry.outputName.empty()) {
      const auto production = sun::driver::readBuildRecord(entry.outputName);
      if (production && !production->hasTests &&
          checkTestFreshness(entry.outputName, inputs, problem).status ==
              "fresh") {
        continue;
      }
    }
    llvm::json::Object suite;
    suite["entrypoint"] = path;
    suite["test_binary"] =
        binary.empty() ? llvm::json::Value(nullptr) : llvm::json::Value(binary);
    // A concurrent rebuild must not transfer an old capability to a new file.
    const bool stable = identity && identity == testBinaryIdentity(binary);
    suite["listing_version"] =
        record && stable ? record->testListingVersion : 0;
    suite["binary_identity"] =
        identity ? llvm::json::Value(*identity) : llvm::json::Value(nullptr);
    suite["freshness"] = freshness.status;
    suite["reason"] = freshness.reason;
    suite["sources"] = std::move(sources);
    suites.push_back(std::move(suite));
  }
  llvm::json::Array paths;
  for (const auto& path : watched) {
    paths.push_back(path);
  }
  llvm::json::Object result;
  result["suites"] = std::move(suites);
  result["watched_paths"] = std::move(paths);
  llvm::json::Array artifacts;
  for (const auto& binary : binaries) {
    artifacts.push_back(binary);
  }
  result["watched_binaries"] = std::move(artifacts);
  llvm::json::Array patterns;
  for (const auto& pattern : configPatterns) {
    patterns.push_back(pattern);
  }
  result["watched_patterns"] = std::move(patterns);
  result["errors"] = std::move(errors);
  return result;
}

}  // namespace sun::lsp
