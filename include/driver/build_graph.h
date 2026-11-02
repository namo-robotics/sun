#pragma once

#include <llvm/Support/JSON.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "driver/sun_config.h"

/** Discovers local package providers and coordinates their staged products. */
namespace sun::driver {

/** Reads the immutable context inherited by custom build commands. */
llvm::json::Object readBuildContext();

/** Restores a child compiler's source settings and already resolved
 * dependencies. */
SunConfig buildContextConfig();

/** A bounded set of project configuration files with a shared output root. */
struct ConfigGraph {
  std::filesystem::path root;
  std::vector<std::filesystem::path> projects;
};

/** Compiles one configured product into its workspace output path. */
using ProductCompiler = std::function<void(
    const SunConfig&, const ConfigEntrypoint&, const std::filesystem::path&)>;

/** Installs the CLI compiler callback; readers can resolve already staged
 * outputs. */
void setProductCompiler(ProductCompiler compiler);

/** Selects the output configuration and whether preparation steps must rerun.
 */
void setBuildConfiguration(const std::string& name, bool force);

/** Registers explicitly selected config roots for read-only editor resolution.
 */
void setConfiguredBuildRoots(const std::vector<std::string>& roots);

/** Loads an explicit root config and its included configs without merging them.
 */
ConfigGraph loadConfigGraph(const std::filesystem::path& file,
                            const std::string& target = "");

/** Makes an explicit config graph available to nested dependency resolution. */
class ConfigGraphScope {
 public:
  /** Selects the graph until this scope ends. */
  explicit ConfigGraphScope(const ConfigGraph& graph);
  /** Restores the enclosing graph. */
  ~ConfigGraphScope();
  /** Prevents copying graph-scope ownership. */
  ConfigGraphScope(const ConfigGraphScope&) = delete;
  /** Prevents assigning graph-scope ownership. */
  ConfigGraphScope& operator=(const ConfigGraphScope&) = delete;

 private:
  const ConfigGraph* previous_;
};

/** Applies the selected root's generated and installed paths to one project. */
SunConfig configureBuildPaths(const ConfigGraph& graph, SunConfig config);

/** Finds a selected project's config for source or generated source
 * directories. */
std::optional<SunConfig> configForBuildDirectory(
    const std::filesystem::path& directory, const std::string& target);

/** Reads an entrypoint's manifest identity without resolving its dependencies.
 */
std::string entrypointIdentity(const ConfigEntrypoint& entry);

/** Builds selected project products, returning their installed package root. */
std::filesystem::path buildConfigProducts(
    const ConfigGraph& workspace, SunConfig config,
    const std::vector<ConfigDependency>& selections = {},
    const std::string& entrypoint = "");

/** Prefers a matching local provider; an absent provider permits external
 * resolution. */
std::optional<std::filesystem::path> resolveLocalDependency(
    const SunConfig& consumer, const std::string& name);

}  // namespace sun::driver
