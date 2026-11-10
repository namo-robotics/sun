/** Language server test suites implementation. */
#include <sys/stat.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

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

/** One build location and the configuration used to resolve its inputs. */
struct TestBuildCandidate {
  sun::driver::ConfigEntrypoint entry;
  std::optional<sun::driver::ConfigGraph> graph;
  std::string packageId;
  std::string location;
  std::set<std::string> configs;
};

/** Order build candidates without depending on their timestamps across roots. */
enum class TestBuildPreference { Missing, Unknown, Stale, Fresh };

/** Evaluated metadata and preference for one build. */
struct TestBuildInspection {
  llvm::json::Object suite;
  TestBuildPreference preference;
  bool hidden;
};

/** Resolve inputs in this build's graph and inspect artifacts without executing them. */
static TestBuildInspection inspectTestBuild(
    const std::string& path, const TestBuildCandidate& candidate,
    const std::string& configurationError, std::set<std::string>& watched,
    std::set<std::string>& binaries) {
  namespace fs = std::filesystem;
  const auto& entry = candidate.entry;
  std::optional<sun::driver::ConfigGraphScope> scope;
  if (candidate.graph) {
    scope.emplace(*candidate.graph);
  }
  std::set<std::string> inputs = candidate.configs;
  std::set<std::string> artifacts;
  inputs.insert(path);
  std::string problem = configurationError;
  try {
    const auto manifest = ManifestProcessor::fromEntrypointFile(path);
    if (manifest) {
      inputs.insert(manifest->sunFiles.begin(), manifest->sunFiles.end());
      inputs.insert(manifest->testSunFiles.begin(),
                    manifest->testSunFiles.end());
      inputs.insert(manifest->protoFiles.begin(), manifest->protoFiles.end());
      for (const auto& moon : manifest->moonImports) {
        artifacts.insert(moon.path);
      }
      for (const auto& archive : manifest->nativeArchives) {
        artifacts.insert(archive.path);
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
  // Rebuilding a library cannot add test declarations to this entrypoint.
  const auto declarationInputs = inputs;
  inputs.insert(artifacts.begin(), artifacts.end());
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
  bool hide = false;
  // Trust non-test metadata while sources and configuration are unchanged,
  // even when dependency artifacts have been rebuilt.
  if (freshness.status != "fresh" && problem.empty() &&
      !entry.outputName.empty()) {
    const auto production = sun::driver::readBuildRecord(entry.outputName);
    if (production && !production->hasTests &&
        checkTestFreshness(entry.outputName, declarationInputs, problem).status ==
            "fresh") {
      hide = true;
    }
  }
  llvm::json::Object suite;
  suite["entrypoint"] = path;
  suite["package_id"] = candidate.packageId;
  suite["entrypoint_name"] =
      entry.name.empty() ? fs::path(path).stem().string() : entry.name;
  suite["build_location"] = candidate.location;
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
  // Fresh builds win; keep configured workspace order on ties. Missing
  // artifacts must not displace an existing stale or unknown build.
  auto preference = TestBuildPreference::Missing;
  if (hide || freshness.status == "fresh") {
    preference = TestBuildPreference::Fresh;
  } else if (freshness.status == "stale") {
    preference = TestBuildPreference::Stale;
  } else if (freshness.status == "unknown") {
    preference = TestBuildPreference::Unknown;
  }
  return {std::move(suite), preference, hide};
}

/** Lists expected test artifacts and checks their inputs without semantic
 * analysis. */
llvm::json::Object testSuites() {
  namespace fs = std::filesystem;
  std::map<std::string, std::vector<TestBuildCandidate>> entries;
  std::set<std::string> configs, configPatterns;
  for (const auto& root : configuredSunConfigs) {
    collectTestConfigPaths(root, configs, configPatterns);
  }
  llvm::json::Array errors;
  sun::driver::setConfiguredBuildRoots(configuredSunConfigs);
  for (const auto& root : configuredSunConfigs) {
    try {
      const auto graph = sun::driver::loadConfigGraph(root);
      std::set<std::string> workspaceConfigs, ignoredPatterns;
      collectTestConfigPaths(root, workspaceConfigs, ignoredPatterns);
      for (const auto& file : graph.projects) {
        collectTestConfigPaths(file.string(), configs, configPatterns);
        const auto standalone = sun::driver::loadConfigGraph(file);
        std::set<std::string> standaloneConfigs;
        collectTestConfigPaths(file.string(), standaloneConfigs, ignoredPatterns);
        for (const auto* candidateGraph : {&graph, &standalone}) {
          const auto config = sun::driver::configureBuildPaths(
              *candidateGraph, sun::driver::SunConfig::loadFile(file));
          for (auto entry : config.entrypoints) {
            if (!entry.git.empty()) {
              continue;
            }
            entry.path = normalizePath(
                ManifestProcessor::expandPathVariables(entry.path, &config));
            entry.testBinaryName = ManifestProcessor::expandPathVariables(
                entry.testBinaryName, &config);
            entry.outputName = ManifestProcessor::expandPathVariables(
                entry.outputName, &config);
            auto& candidates = entries[entry.path];
            bool duplicate = false;
            for (const auto& existing : candidates) {
              if (existing.entry.testBinaryName == entry.testBinaryName) {
                duplicate = true;
                break;
              }
            }
            if (!duplicate) {
              candidates.push_back(
                  {std::move(entry), *candidateGraph, config.packageId,
                   candidateGraph->root == config.configDir ? "standalone"
                                                           : "workspace",
                   candidateGraph == &graph ? workspaceConfigs
                                            : standaloneConfigs});
            }
          }
        }
      }
    } catch (const std::exception& error) {
      errors.push_back(root + ": " + error.what());
    }
  }
  for (const auto& file : explicitEntrypoints) {
    sun::driver::ConfigEntrypoint entry;
    entry.path = normalizePath(file);
    if (!entries.count(entry.path)) {
      entries[entry.path].push_back(
          {std::move(entry), std::nullopt, "", "unconfigured", configs});
    }
  }
  // Match the existing editor behavior for open manifest entrypoints.
  for (const auto& entry : entrypointManager.entrypoints()) {
    if (!openSourceOverrides.count(entry.entrypointPath)) {
      continue;
    }
    sun::driver::ConfigEntrypoint candidate;
    candidate.path = entry.entrypointPath;
    if (!entries.count(candidate.path)) {
      entries[candidate.path].push_back(
          {std::move(candidate), std::nullopt, "", "unconfigured", configs});
    }
  }

  llvm::json::Array suites;
  std::set<std::string> watched = configs;
  std::set<std::string> binaries;
  for (const auto& [path, candidates] : entries) {
    std::optional<TestBuildInspection> selected;
    for (const auto& candidate : candidates) {
      auto inspected = inspectTestBuild(
          path, candidate,
          errors.empty() ? "" : "Cannot resolve project configuration", watched,
          binaries);
      if (!selected || inspected.preference > selected->preference) {
        selected = std::move(inspected);
      }
    }
    if (selected && !selected->hidden) {
      suites.push_back(std::move(selected->suite));
    }
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
