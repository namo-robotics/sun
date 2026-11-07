/** Language server configuration implementation. */

#include <exception>
#include <filesystem>
#include <iostream>
#include <utility>

#include "driver/build_graph.h"
#include "driver/manifest_processor.h"
#include "lsp/declarations.h"
#include "server.h"
#include "support/sun_path.h"

using sun::driver::ManifestProcessor;

/** Implements the Sun language server. */
namespace sun::lsp {

/** Current editor buffers, used to resolve manifests and analyze unsaved edits.
 */
std::map<std::string, std::string> openSourceOverrides;

void EntrypointManager::setEntrypoints(const std::vector<std::string>& paths) {
  configuredPaths_ = paths;
  entrypoints_.clear();
  fileToEntrypoint_.clear();

  std::set<std::string> seen;
  for (const auto& path : configuredPaths_) {
    if (!seen.insert(normalizePath(path)).second) {
      continue;
    }
    auto config = parseEntrypoint(path);
    if (config) {
      // Map all covered files to this entrypoint
      for (const auto& file : config->coveredFiles) {
        // First entrypoint wins if file appears in multiple manifests
        if (fileToEntrypoint_.find(file) == fileToEntrypoint_.end()) {
          fileToEntrypoint_[file] = entrypoints_.size();
        }
      }
      entrypoints_.push_back(std::move(*config));
    }
  }
  // A manifest file uses its own context even if another manifest includes
  // it.
  for (size_t i = 0; i < entrypoints_.size(); ++i) {
    fileToEntrypoint_[entrypoints_[i].entrypointPath] = i;
  }
}

const EntrypointConfig* EntrypointManager::findEntrypointForFile(
    const std::string& filePath) const {
  auto it = fileToEntrypoint_.find(normalizePath(filePath));
  if (it != fileToEntrypoint_.end()) {
    return &entrypoints_[it->second];
  }
  return nullptr;
}

const std::vector<EntrypointConfig>& EntrypointManager::entrypoints() const {
  return entrypoints_;
}

std::optional<EntrypointConfig> EntrypointManager::parseEntrypoint(
    const std::string& path) {
  std::filesystem::path entrypointPath;
  try {
    entrypointPath = normalizePath(path);
  } catch (const std::filesystem::filesystem_error&) {
    // The path cannot be normalized
    return std::nullopt;
  }

  std::optional<sun::driver::ResolvedManifest> resolved;
  try {
    auto buffer = openSourceOverrides.find(entrypointPath.string());
    if (buffer == openSourceOverrides.end()) {
      resolved = ManifestProcessor::fromEntrypointFile(entrypointPath.string());
    } else {
      auto parser = sun::parsing::Parser::createStringParser(buffer->second);
      parser.setFilePath(entrypointPath.string());
      parser.setBaseDir(entrypointPath.parent_path().string());
      auto ast = parser.parseProgram();
      if (const auto* manifest = ManifestProcessor::findManifest(*ast)) {
        resolved = ManifestProcessor::process(
            *manifest, entrypointPath.parent_path().string());
      }
    }
  } catch (const std::exception&) {
    // A failed moon download degrades to per-file mode instead of
    // crashing the server.
    return std::nullopt;
  }
  if (!resolved) {
    return std::nullopt;
  }

  // Build entrypoint config
  EntrypointConfig config;
  config.entrypointPath = entrypointPath.string();
  config.sunFiles = std::move(resolved->sunFiles);
  // The editor always analyzes test code too: tests and their test_files
  // helpers get diagnostics/hover like any other source.
  config.sunFiles.insert(config.sunFiles.end(), resolved->testSunFiles.begin(),
                         resolved->testSunFiles.end());
  config.moonImports = std::move(resolved->moonImports);
  config.protoFiles = std::move(resolved->protoFiles);

  for (const auto& file : config.sunFiles) {
    if (std::filesystem::exists(file) ||
        openSourceOverrides.count(normalizePath(file))) {
      config.coveredFiles.insert(normalizePath(file));
    }
  }

  // The entrypoint file itself is also covered
  config.sunFiles.insert(config.sunFiles.begin(), entrypointPath.string());
  config.coveredFiles.insert(entrypointPath.string());

  return config;
}
EntrypointManager entrypointManager;

// Config files the editor pointed the server at (the sun_configs setting).
// Their declared entrypoints merge with any explicitly configured ones, and
// their build metadata (test binary names) feeds the workspace test
// listing.
std::vector<std::string> configuredSunConfigs;
std::vector<std::string> explicitEntrypoints;
std::map<std::string, sun::driver::ConfigEntrypoint> configEntrypointInfo;

/**
 * Re-read the configured sun-config files and hand the manager the union of
 * their entrypoints, explicitly configured ones, and open manifest files
 * (configs first, so a file covered by both maps to the config's entrypoint). A
 * config that fails to parse is skipped: a bad editor setting must not kill the
 * server.
 */
void refreshEntrypoints() {
  std::vector<std::string> combined;
  configEntrypointInfo.clear();
  std::vector<std::string> validConfigRoots;
  for (const auto& configPath : configuredSunConfigs) {
    try {
      auto graph = sun::driver::loadConfigGraph(configPath);
      for (const auto& project : graph.projects) {
        sun::driver::SunConfig config = sun::driver::configureBuildPaths(
            graph, sun::driver::SunConfig::loadFile(project));
        for (auto& entry : config.entrypoints) {
          // Project discovery never fetches remote source repositories.
          if (!entry.git.empty()) {
            continue;
          }
          entry.path =
              ManifestProcessor::expandPathVariables(entry.path, &config);
          entry.testBinaryName = ManifestProcessor::expandPathVariables(
              entry.testBinaryName, &config);
          combined.push_back(entry.path);
          configEntrypointInfo.emplace(normalizePath(entry.path), entry);
        }
      }
      validConfigRoots.push_back(configPath);
    } catch (const std::exception& e) {
      std::cerr << "sun-lsp: ignoring " << configPath << ": " << e.what()
                << std::endl;
    }
  }
  sun::driver::setConfiguredBuildRoots(validConfigRoots);
  combined.insert(combined.end(), explicitEntrypoints.begin(),
                  explicitEntrypoints.end());
  for (const auto& [path, source] : openSourceOverrides) {
    combined.push_back(path);
  }
  entrypointManager.setEntrypoints(combined);
}

/**
 * Install manifest path variables ($NAME) from a configuration object's
 * "pathVariables" member ({NAME: dir, ...}). Replaces the previous set so
 * removed variables disappear. Returns true if the member was present.
 */
bool applyPathVariables(const llvm::json::Object& config) {
  const llvm::json::Object* vars = config.getObject("pathVariables");
  if (!vars) {
    return false;
  }
  ManifestProcessor::clearPathVariables();
  for (const auto& [name, value] : *vars) {
    if (auto str = value.getAsString()) {
      ManifestProcessor::setDefaultPathVariable(llvm::StringRef(name).str(),
                                                str->str());
    }
  }
  return true;
}

/**
 * Read a configuration object's "entrypoints" member. Entries may be strings
 * or {path: string} objects. Returns nullopt if the member is absent.
 */
std::optional<std::vector<std::string>> readEntrypoints(
    const llvm::json::Object& config) {
  const llvm::json::Array* entrypoints = config.getArray("entrypoints");
  if (!entrypoints) {
    return std::nullopt;
  }
  std::vector<std::string> paths;
  for (const auto& entry : *entrypoints) {
    if (auto str = entry.getAsString()) {
      paths.push_back(str->str());
    } else if (const llvm::json::Object* entryObj = entry.getAsObject()) {
      if (auto path = entryObj->getString("path")) {
        paths.push_back(path->str());
      }
    }
  }
  return paths;
}

/**
 * The sun_configs setting: sun-config.json files whose entrypoints the
 * server should know about. Paths arrive absolute from the client.
 */
std::optional<std::vector<std::string>> readSunConfigs(
    const llvm::json::Object& config) {
  const llvm::json::Array* configs = config.getArray("sun_configs");
  if (!configs) {
    return std::nullopt;
  }
  std::vector<std::string> paths;
  for (const auto& entry : *configs) {
    if (auto str = entry.getAsString()) {
      paths.push_back(str->str());
    }
  }
  return paths;
}

}  // namespace sun::lsp
