#include "driver/build_graph.h"

#include <fnmatch.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "abi/nominal_identity.h"
#include "driver/manifest_processor.h"
#include "driver/package.h"
#include "driver/system_dependencies.h"
#include "moon_bundling/moon.h"
#include "support/error.h"

/** Discovers local package providers and coordinates their staged products. */
namespace sun::driver {
/** Keeps workspace discovery and active build state private. */
namespace {
/** Shortens filesystem paths used throughout the resolver. */
namespace fs = std::filesystem;
thread_local ProductCompiler compiler;
thread_local std::string configuration = "release";
thread_local bool forceSteps = false;
std::mutex configuredRootsMutex;
std::vector<std::string> configuredRoots;
thread_local const ConfigGraph* currentGraph = nullptr;
thread_local std::set<std::string> active;
thread_local std::map<std::string, std::set<std::string>> selectedProducts;
thread_local std::map<std::string, fs::path> resolvedProducts;

/** Reports actionable workspace failures as compilation diagnostics. */
[[noreturn]] void fail(const std::string& message) {
  sun::support::logAndThrowError("build graph: " + message);
}

/** Removes a build from the cycle detector on success or failure. */
struct BuildGuard {
  std::string key;
  /** Registers a project and rejects recursive dependency graphs. */
  explicit BuildGuard(std::string value) : key(std::move(value)) {
    if (!active.insert(key).second) {
      fail("dependency cycle at " + key);
    }
  }
  /** Restores the graph after a build attempt. */
  ~BuildGuard() { active.erase(key); }
};

/** Hashes declared files in deterministic order, including directory contents.
 */
std::string hashPaths(const std::vector<fs::path>& paths) {
  std::string contents;
  for (const auto& path : paths) {
    std::vector<fs::path> files;
    if (fs::is_directory(path)) {
      for (const auto& item : fs::recursive_directory_iterator(path)) {
        if (item.is_regular_file()) {
          files.push_back(item.path());
        }
      }
      std::sort(files.begin(), files.end());
    } else {
      files.push_back(path);
    }
    for (const auto& file : files) {
      if (!fs::is_regular_file(file)) {
        fail("missing step input or output: " + file.string());
      }
      contents += file.string() + "\n" + packageFileHash(file) + "\n";
    }
  }
  return sun::moon_bundling::computeSha256Hex(contents);
}

/** Runs declared preparation steps before a product, using host tools
 * explicitly. */
void runSteps(const SunConfig& config, const ConfigEntrypoint& entry,
              const fs::path& output, const fs::path& artifact) {
  std::set<std::string> visiting, complete;
  std::function<void(const std::string&)> run = [&](const std::string& name) {
    if (complete.count(name)) {
      return;
    }
    if (!visiting.insert(name).second) {
      fail("build step cycle at " + name);
    }
    auto found =
        std::find_if(config.steps.begin(), config.steps.end(),
                     [&](const auto& step) { return step.name == name; });
    if (found == config.steps.end()) {
      fail("unknown build step: " + name);
    }
    for (const auto& prerequisite : found->prerequisites) {
      run(prerequisite);
    }
    const auto expand = [&](const std::string& value) {
      return ManifestProcessor::expandPathVariables(value, &config);
    };
    const fs::path working = expand(found->workingDirectory);
    std::vector<std::string> arguments;
    for (size_t i = 0; i < found->command.size(); ++i) {
      arguments.push_back(i == 0 && found->command[i].starts_with("${")
                              ? found->command[i]
                              : expand(found->command[i]));
    }
    // Tool references resolve with host settings while their inputs and outputs
    // retain the target project's paths.
    if (found->command.front().starts_with("${")) {
      auto host = SunConfig::loadFile(config.configDir / "sun-config.json",
                                      llvm::sys::getDefaultTargetTriple());
      for (const auto& [key, value] : config.pathVariables) {
        host.pathVariables[key] = value;
      }
      arguments[0] =
          ManifestProcessor::expandPathVariables(found->command.front(), &host);
    }
    auto program = llvm::sys::findProgramByName(arguments.front());
    if (!program) {
      fail("build tool not found: " + arguments.front());
    }
    arguments[0] = *program;
    std::vector<fs::path> inputs{*program}, outputs;
    for (const auto& path : found->inputs) {
      inputs.push_back(working / expand(path));
    }
    for (const auto& path : found->outputs) {
      auto absolute = fs::absolute(working / expand(path)).lexically_normal();
      auto relative = absolute.lexically_relative(output);
      if (relative.empty() || *relative.begin() == "..") {
        fail("step outputs must be beneath SUN_BUILD: " + absolute.string());
      }
      outputs.push_back(absolute);
    }
    std::string fingerprint = working.string() + "\n" + hashPaths(inputs);
    for (const auto& argument : arguments) {
      fingerprint += "\n" + argument;
    }
    for (const auto& [key, value] : found->environment) {
      fingerprint += "\n" + key + "=" + expand(value);
    }
    auto stamp = output / (".step-" + name);
    std::string previous;
    if (fs::is_regular_file(stamp)) {
      std::ifstream file(stamp);
      std::getline(file, previous);
    }
    const auto digest = sun::moon_bundling::computeSha256Hex(fingerprint);
    bool exist = std::all_of(outputs.begin(), outputs.end(),
                             [](const auto& path) { return fs::exists(path); });
    if (!forceSteps && exist && previous == digest + ":" + hashPaths(outputs)) {
      visiting.erase(name);
      complete.insert(name);
      return;
    }
    for (const auto& path : outputs) {
      fs::create_directories(path.parent_path());
    }
    /** Restores process context after a synchronous external build action. */
    struct ProcessScope {
      fs::path directory = fs::current_path();
      std::map<std::string, std::optional<std::string>> environment;
      /** Restores the working directory and explicitly changed variables. */
      ~ProcessScope() {
        std::error_code ignored;
        fs::current_path(directory, ignored);
        for (const auto& [key, value] : environment) {
          if (value) {
            setenv(key.c_str(), value->c_str(), 1);
          } else {
            unsetenv(key.c_str());
          }
        }
      }
    } scope;
    for (const auto& [key, value] : found->environment) {
      const char* old = std::getenv(key.c_str());
      scope.environment[key] =
          old ? std::optional<std::string>(old) : std::nullopt;
      setenv(key.c_str(), expand(value).c_str(), 1);
    }
    fs::current_path(working);
    std::vector<llvm::StringRef> refs(arguments.begin(), arguments.end());
    const std::string project = config.packageId.empty()
                                    ? config.configDir.string()
                                    : config.packageId;
    sun::support::messageStream("info")
        << "Building " << artifact.filename().string() << ": step " << name
        << "\n";
    // Show the step before the child process writes any output.
    llvm::outs().flush();
    if (llvm::sys::ExecuteAndWait(*program, refs) != 0) {
      fail("build step failed: " + project + ": " + name);
    }
    std::ofstream(stamp) << digest << ":" << hashPaths(outputs) << "\n";
    visiting.erase(name);
    complete.insert(name);
  };
  for (const auto& name : entry.steps) {
    run(name);
  }
}

/** Expands explicit config patterns one path component at a time. */
void expandPattern(const fs::path& base, fs::path::const_iterator item,
                   fs::path::const_iterator end,
                   std::vector<fs::path>& matches) {
  if (item == end) {
    if (fs::is_regular_file(base)) {
      matches.push_back(fs::canonical(base));
    }
    return;
  }
  auto component = item->string();
  auto next = std::next(item);
  if (component.find_first_of("*?[") == std::string::npos) {
    expandPattern(base / *item, next, end, matches);
  } else if (fs::is_directory(base)) {
    for (const auto& child : fs::directory_iterator(base)) {
      if (child.is_symlink() ||
          child.path().filename().string().starts_with('.')) {
        continue;
      }
      if (fnmatch(component.c_str(), child.path().filename().c_str(), 0) == 0) {
        expandPattern(child.path(), next, end, matches);
      }
    }
  }
}

/** Visits included configs with cycle detection and deterministic ordering. */
void visitConfig(const fs::path& input, const std::string& target,
                 ConfigGraph& graph, std::set<fs::path>& visiting,
                 std::set<fs::path>& visited,
                 std::map<std::string, fs::path>& ids) {
  auto file = fs::canonical(input);
  if (visiting.count(file)) {
    fail("config inclusion cycle at " + file.string());
  }
  if (visited.count(file)) {
    return;
  }
  visiting.insert(file);
  auto config = SunConfig::loadFile(file, target);
  if (!config.targetSupported) {
    sun::support::logWarning(
        "Skipping project " + sun::support::displayPath(file.string()) +
        ": target " + configTargetKey(config.targetTriple) +
        " is not in supported_targets");
    graph.skippedProjects.push_back(file);
    visiting.erase(file);
    visited.insert(file);
    return;
  }
  if (!config.packageId.empty() &&
      !ids.emplace("package:" + config.packageId, file).second) {
    fail("duplicate package_id: " + config.packageId);
  }
  for (const auto& entry : config.entrypoints) {
    const auto identity = config.packageId.empty()
                              ? entry.name
                              : config.packageId + "/" + entry.name;
    sun::abi::validateLibraryId(identity);
    if (!ids.emplace("artifact:" + identity, file).second) {
      fail("duplicate artifact id: " + identity);
    }
  }
  graph.projects.push_back(file);
  for (const auto& patternString : config.projects) {
    fs::path pattern(patternString);
    std::vector<fs::path> matches;
    expandPattern({}, pattern.begin(), pattern.end(), matches);
    std::sort(matches.begin(), matches.end());
    if (matches.empty() &&
        patternString.find_first_of("*?[") == std::string::npos) {
      fail("included config does not exist: " + patternString);
    }
    for (const auto& match : matches) {
      visitConfig(match, target, graph, visiting, visited, ids);
    }
  }
  visiting.erase(file);
  visited.insert(file);
}
}  // namespace

void setProductCompiler(ProductCompiler callback) {
  compiler = std::move(callback);
}

void setBuildConfiguration(const std::string& name, bool force) {
  if (name.empty() ||
      name.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
          std::string::npos) {
    fail("invalid build configuration");
  }
  configuration = name;
  forceSteps = force;
}

void setConfiguredBuildRoots(const std::vector<std::string>& roots) {
  std::lock_guard<std::mutex> lock(configuredRootsMutex);
  configuredRoots = roots;
}

ConfigGraph loadConfigGraph(const fs::path& file, const std::string& target) {
  ConfigGraph graph{fs::weakly_canonical(fs::absolute(file).parent_path()), {}};
  std::set<fs::path> visiting, visited;
  std::map<std::string, fs::path> ids;
  visitConfig(file, target, graph, visiting, visited, ids);
  return graph;
}

ConfigGraphScope::ConfigGraphScope(const ConfigGraph& graph)
    : previous_(currentGraph) {
  if (!currentGraph) {
    selectedProducts.clear();
    resolvedProducts.clear();
  }
  currentGraph = &graph;
}
ConfigGraphScope::~ConfigGraphScope() { currentGraph = previous_; }

fs::path configWorkspaceRoot(const SunConfig& config) {
  return currentGraph ? currentGraph->root : config.configDir;
}

std::vector<std::string> defaultConfigEntrypoints(const SunConfig& config) {
  std::vector<std::string> names;
  for (const auto& entry : config.entrypoints) {
    if (entry.exported || config.packageId.empty()) {
      names.push_back(entry.name);
    }
  }
  return names;
}

std::vector<const ConfigEntrypoint*> findConfigEntrypoints(
    const SunConfig& config, const std::vector<std::string>& names) {
  std::vector<const ConfigEntrypoint*> entries;
  for (const auto& name : names) {
    auto found =
        std::find_if(config.entrypoints.begin(), config.entrypoints.end(),
                     [&](const auto& entry) { return entry.name == name; });
    if (found == config.entrypoints.end()) {
      fail("unknown entrypoint " + name);
    }
    entries.push_back(&*found);
  }
  return entries;
}

llvm::json::Object readBuildContext() {
  const char* path = std::getenv("SUN_BUILD_CONTEXT");
  if (!path || !*path) {
    fail("this command requires a custom build context");
  }
  auto bytes = llvm::MemoryBuffer::getFile(path);
  if (!bytes) {
    fail("cannot read build context");
  }
  auto parsed = llvm::json::parse((*bytes)->getBuffer());
  if (!parsed) {
    fail("invalid build context: " + llvm::toString(parsed.takeError()));
  }
  auto* object = parsed->getAsObject();
  if (!object || object->getInteger("version") != 1 ||
      !object->getString("config") || !object->getString("target") ||
      !object->getObject("variables") || !object->getObject("dependencies") ||
      !object->getArray("sun_path")) {
    fail("incomplete build context");
  }
  return std::move(*object);
}

SunConfig buildContextConfig() {
  auto context = readBuildContext();
  auto config = SunConfig::loadFile(context.getString("config")->str(),
                                    context.getString("target")->str());
  config.dependencies.clear();
  for (auto key : {"variables", "dependencies"}) {
    for (const auto& [name, value] : *context.getObject(key)) {
      if (!value.getAsString()) {
        fail("invalid build context path");
      }
      config.pathVariables[name.str()] = value.getAsString()->str();
    }
  }
  config.sunPath.clear();
  for (const auto& path : *context.getArray("sun_path")) {
    if (!path.getAsString()) {
      fail("invalid build context search path");
    }
    config.sunPath.push_back(path.getAsString()->str());
  }
  return config;
}

SunConfig configureBuildPaths(const ConfigGraph& graph, SunConfig config) {
  const auto projectName = config.packageId.empty()
                               ? config.configDir.filename().string()
                               : config.packageId;
  const auto target = configTargetKey(config.targetTriple);
  const auto& buildConfig = configuration;
  const auto output =
      graph.root / ".sun/build" / target / buildConfig / projectName;
  const auto install =
      graph.root / ".sun/install" / target / buildConfig / projectName;
  config.pathVariables["SUN_JOBS"] =
      std::to_string(std::max(1u, std::thread::hardware_concurrency() / 2));
  config.pathVariables["SUN_BUILD"] = output.string();
  config.pathVariables["SUN_INSTALL"] = install.string();
  config.sunPath.insert(config.sunPath.begin(), output.string());
  for (auto& entry : config.entrypoints) {
    const auto name =
        entry.name.empty() ? fs::path(entry.path).stem().string() : entry.name;
    entry.outputName =
        (output /
         (name +
          (entry.type == ConfigEntrypoint::Type::Library ? ".moon" : "")))
            .string();
    entry.testBinaryName = (output / (name + "_test")).string();
  }
  return config;
}

std::optional<SunConfig> configForBuildDirectory(const fs::path& directory,
                                                 const std::string& target) {
  std::vector<ConfigGraph> graphs;
  if (currentGraph) {
    graphs.push_back(*currentGraph);
  } else {
    std::vector<std::string> roots;
    {
      std::lock_guard<std::mutex> lock(configuredRootsMutex);
      roots = configuredRoots;
    }
    for (const auto& root : roots) {
      graphs.push_back(loadConfigGraph(root, target));
    }
  }
  std::optional<SunConfig> result;
  size_t specificity = 0;
  const auto source = fs::weakly_canonical(directory);
  for (const auto& graph : graphs) {
    for (const auto& file : graph.projects) {
      auto config =
          configureBuildPaths(graph, SunConfig::loadFile(file, target));
      for (const auto& root :
           {config.configDir, fs::path(config.pathVariables.at("SUN_BUILD"))}) {
        auto relative = source.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") {
          continue;
        }
        if (root.string().size() > specificity) {
          specificity = root.string().size();
          result = config;
        }
      }
    }
  }
  return result;
}

/** Keeps build ordering and output paths private to this file. */
namespace {
/** Orders the selected products after their prerequisites and rejects cycles.
 */
std::vector<const ConfigEntrypoint*> orderPrerequisites(
    const SunConfig& config,
    const std::vector<const ConfigEntrypoint*>& entries) {
  std::vector<const ConfigEntrypoint*> ordered;
  std::set<std::string> visitingEntries, builtEntries;
  std::function<void(const ConfigEntrypoint*)> order =
      [&](const ConfigEntrypoint* entry) {
        if (builtEntries.count(entry->name)) {
          return;
        }
        if (!visitingEntries.insert(entry->name).second) {
          fail("entrypoint dependency cycle: " + entry->name);
        }
        for (const auto* dependency :
             findConfigEntrypoints(config, entry->prerequisites)) {
          order(dependency);
        }
        visitingEntries.erase(entry->name);
        builtEntries.insert(entry->name);
        ordered.push_back(entry);
      };
  for (const auto* entry : entries) {
    order(entry);
  }
  return ordered;
}

/** Computes the compiler output path for a configured library or executable. */
fs::path productOutput(const SunConfig& config, const ConfigEntrypoint& entry) {
  auto output =
      entry.outputName.empty()
          ? fs::path(config.pathVariables.at("SUN_BUILD")) /
                (entry.name +
                 (entry.type == ConfigEntrypoint::Type::Library ? ".moon" : ""))
          : fs::path(ManifestProcessor::expandPathVariables(entry.outputName,
                                                            &config));
  if (entry.type == ConfigEntrypoint::Type::Library &&
      output.extension() != ".moon") {
    output += ".moon";
  }
  return output;
}
/** Runs preparation and compilation; returns false for a test-only product. */
bool compileProduct(const SunConfig& config, const ConfigEntrypoint& entry,
                    const fs::path& artifact) {
  const fs::path output = config.pathVariables.at("SUN_BUILD");
  fs::create_directories(output);
  fs::create_directories(artifact.parent_path());
  runSteps(config, entry, output, artifact);
  compiler(config, entry, artifact);
  if (fs::is_regular_file(artifact)) {
    return true;
  }
  const auto testOutput = entry.testBinaryName.empty()
                              ? artifact.string() + "_test"
                              : ManifestProcessor::expandPathVariables(
                                    entry.testBinaryName, &config);
  if (!entry.exported && !isCurrentlyBuildingDependency() &&
      fs::is_regular_file(testOutput)) {
    return false;
  }
  fail("build did not produce " + artifact.string());
}
}  // namespace

fs::path buildConfigProducts(const ConfigGraph& workspace, SunConfig config,
                             const std::vector<std::string>& names) {
  config = configureBuildPaths(workspace, std::move(config));
  const auto projectName = config.packageId.empty()
                               ? config.configDir.filename().string()
                               : config.packageId;
  const auto projectFile =
      config.configFile.empty() ? config.configDir : config.configFile;
  BuildGuard guard(workspace.root.string() + "@" + projectFile.string() + "@" +
                   configTargetKey(config.targetTriple));
  const fs::path install = config.pathVariables.at("SUN_INSTALL");
  auto entries = findConfigEntrypoints(config, names);
  if (entries.empty()) {
    return install;
  }
  checkSystemDependencies(config);
  auto& accumulated = selectedProducts[guard.key];
  for (const auto* entry : entries) {
    accumulated.insert(entry->name);
  }
  entries.clear();
  for (const auto& entry : config.entrypoints) {
    if (accumulated.count(entry.name)) {
      entries.push_back(&entry);
    }
  }
  entries = orderPrerequisites(config, entries);
  PackagePlan plan;
  plan.name = fs::path(projectName).filename().string();
  plan.output = install;
  plan.resources = config.exports;
  for (auto& resource : plan.resources) {
    resource.path =
        ManifestProcessor::expandPathVariables(resource.path, &config);
  }
  for (const auto* entry : entries) {
    const bool library = entry->type == ConfigEntrypoint::Type::Library;
    const auto name = entry->name.empty()
                          ? fs::path(entry->path).stem().string()
                          : entry->name;
    auto artifact = productOutput(config, *entry);
    auto dest =
        install / (library ? "lib" : "bin") / (name + (library ? ".moon" : ""));
    if (!compiler) {
      if (!fs::is_regular_file(dest)) {
        fail("build local dependency first with sun build " + config.packageId);
      }
      continue;
    }
    if (!compileProduct(config, *entry, artifact)) {
      continue;
    }
    plan.artifacts.push_back({name, artifact.string(), library});
    for (auto resource : packageEntrypointResources(config, *entry)) {
      resource.path =
          ManifestProcessor::expandPathVariables(resource.path, &config);
      plan.resources.push_back(std::move(resource));
    }
  }
  if (compiler) {
    buildPackages(config, {plan}, false);
  }
  return install;
}

void packageConfigProducts(const ConfigGraph& workspace, SunConfig config,
                           const std::vector<std::string>& names) {
  if (config.packages.empty()) {
    return;
  }
  config = configureBuildPaths(workspace, std::move(config));
  std::vector<PackageArtifact> artifacts;
  for (const auto* entry :
       orderPrerequisites(config, findConfigEntrypoints(config, names))) {
    artifacts.push_back({entry->name, productOutput(config, *entry).string(),
                         entry->type == ConfigEntrypoint::Type::Library});
  }
  buildPackages(config, planPackages(config, artifacts));
}

std::optional<fs::path> resolveLocalDependency(const SunConfig& consumer,
                                               const std::string& name) {
  std::optional<ConfigGraph> ownGraph;
  if (!currentGraph) {
    std::vector<std::string> roots;
    {
      std::lock_guard<std::mutex> lock(configuredRootsMutex);
      roots = configuredRoots;
    }
    for (const auto& root : roots) {
      auto candidate = loadConfigGraph(root, consumer.targetTriple);
      auto consumerFile =
          fs::weakly_canonical(consumer.configDir / "sun-config.json");
      if (std::find(candidate.projects.begin(), candidate.projects.end(),
                    consumerFile) != candidate.projects.end()) {
        if (ownGraph) {
          fail("consumer belongs to multiple configured build roots");
        }
        ownGraph = std::move(candidate);
      }
    }
    auto file = consumer.configDir / "sun-config.json";
    if (!fs::is_regular_file(file)) {
      return std::nullopt;
    }
    if (!ownGraph) {
      ownGraph = loadConfigGraph(file, consumer.targetTriple);
    }
  }
  const auto& graph = currentGraph ? *currentGraph : *ownGraph;
  ConfigGraphScope scope(graph);
  std::string key = graph.root.string() + "\n" + name + "\n" +
                    configTargetKey(consumer.targetTriple);
  const auto& requested = consumer.dependencies.at(name).entrypoints;
  for (const auto& entrypoint : requested) {
    key += "\n" + entrypoint;
  }
  if (auto cached = resolvedProducts.find(key);
      cached != resolvedProducts.end()) {
    return cached->second;
  }
  for (const auto& file : graph.projects) {
    auto config = SunConfig::loadFile(file, consumer.targetTriple);
    if (config.packageId != name) {
      continue;
    }
    auto names =
        requested.empty() ? defaultConfigEntrypoints(config) : requested;
    auto installed = buildConfigProducts(graph, std::move(config), names);
    resolvedProducts.emplace(key, installed);
    return installed;
  }
  return std::nullopt;
}
}  // namespace sun::driver
