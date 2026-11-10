#include "driver/git_source.h"

#include <fcntl.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#include "driver/build_graph.h"
#include "driver/manifest_processor.h"
#include "moon_bundling/moon.h"
#include "support/error.h"

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {
/** Keeps checkout and process helpers private to this file. */
namespace {
/** Settings inherited by source dependencies during the current build. */
thread_local sun::moon_bundling::MoonBuildOptions dependencyOptions;
/** Whether the current command requests fresh source snapshots. */
thread_local bool refreshDependencies = false;
/** Counts nested build scopes so refreshes are shared within a command. */
thread_local unsigned buildDepth = 0;
/** Repository revisions already refreshed during the current command. */
thread_local std::set<std::string> refreshedDependencies;
/** Completed explicit repository builds reused within one compiler command. */
thread_local std::map<std::filesystem::path,
                      std::map<std::string, std::filesystem::path>>
    repositoryProducts;
/** Repository/config selections currently being built, for cycle detection. */
thread_local std::set<std::string> activeDependencies;

/** Runs Git with separate arguments, disabled hooks, and restricted protocols.
 */
void runGit(const std::vector<std::string>& arguments) {
  auto program = llvm::sys::findProgramByName("git");
  if (!program) {
    sun::support::logAndThrowError(
        "Git source entrypoints require git on PATH");
  }
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
    if (*cache) {
      return std::filesystem::absolute(cache);
    }
  }
  return std::filesystem::current_path() / ".sun/cache/git";
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
  if (entry.type != ConfigEntrypoint::Type::Library) {
    sun::support::logAndThrowError("Git entrypoints must have type 'library'");
  }
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
      if (!ready) {
        sun::support::logAndThrowError("could not write Git cache marker");
      }
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
      !fs::is_regular_file(source)) {
    sun::support::logAndThrowError(
        "Git entrypoint is missing or escapes its checkout: " + entry.path);
  }
  return source.string();
}
GitDependencyBuildScope::GitDependencyBuildScope(bool debugInfo, bool optimize,
                                                 bool forceRebuild,
                                                 bool refresh)
    : settings_(dependencyOptions),
      refresh_(refreshDependencies, refresh || refreshDependencies) {
  if (buildDepth++ == 0) {
    refreshedDependencies.clear();
    repositoryProducts.clear();
  }
  dependencyOptions.debugInfo = debugInfo;
  dependencyOptions.optimize = optimize;
  dependencyOptions.forceRebuild = forceRebuild;
}

GitDependencyBuildScope::~GitDependencyBuildScope() { --buildDepth; }

/** Keeps dependency lifetime guards private to this file. */
namespace {
/** Detects cycles and serializes complete dependency graphs across processes.
 */
class DependencyBuildGuard {
  std::string key_;
  int lock_ = -1;

 public:
  /** Registers the selection before any recursive build or lock acquisition. */
  explicit DependencyBuildGuard(std::string key) : key_(std::move(key)) {
    if (activeDependencies.count(key_)) {
      sun::support::logAndThrowError("cyclic Git library dependency");
    }
    if (!isCurrentlyBuildingGitDependency()) {
      const auto cache = cacheDirectory();
      std::filesystem::create_directories(cache);
      lock_ = open((cache / ".build.lock").c_str(),
                   O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (lock_ < 0 || flock(lock_, LOCK_EX) != 0) {
        if (lock_ >= 0) {
          close(lock_);
        }
        sun::support::logAndThrowError("could not lock Git dependency cache");
      }
    }
    activeDependencies.insert(key_);
  }
  /** Releases cycle membership and the outer graph lock on every exit path. */
  ~DependencyBuildGuard() {
    activeDependencies.erase(key_);
    if (lock_ >= 0) {
      close(lock_);
    }
  }
};

/** Applies the chosen config while retaining explicit command-line overrides.
 */
class SourceConfigScope {
  const SunConfig* previous_;
  std::map<std::string, std::string> variables_;

 public:
  /** Replaces any enclosing dependency's config and project overrides. */
  explicit SourceConfigScope(const SunConfig& config)
      : previous_(ManifestProcessor::exchangeSourceConfig(&config)),
        variables_(ManifestProcessor::exchangeDependencyPathVariables({})) {}
  /** Restores the enclosing library's configuration, including after errors. */
  ~SourceConfigScope() {
    ManifestProcessor::exchangeSourceConfig(previous_);
    ManifestProcessor::exchangeDependencyPathVariables(std::move(variables_));
  }
};

/** Checks that named products use sources in the checkout or build directory.
 */
void validateGitProducts(const SunConfig& config,
                         const std::vector<std::string>& names,
                         const std::filesystem::path& checkout) {
  for (const auto* entry : findConfigEntrypoints(config, names)) {
    if (!entry->git.empty()) {
      sun::support::logAndThrowError(
          "selected product must belong to its dependency repository");
    }
    const auto source = std::filesystem::weakly_canonical(
        ManifestProcessor::expandPathVariables(entry->path, &config));
    const auto relative = source.lexically_relative(checkout);
    const auto generated = source.lexically_relative(
        std::filesystem::path(config.pathVariables.at("SUN_BUILD")));
    if ((relative.empty() || *relative.begin() == "..") &&
        (generated.empty() || *generated.begin() == "..")) {
      sun::support::logAndThrowError("dependency source escapes its checkout");
    }
  }
}
}  // namespace

bool isCurrentlyBuildingGitDependency() { return !activeDependencies.empty(); }

const sun::moon_bundling::MoonBuildOptions* gitDependencyBuildOptions() {
  return isCurrentlyBuildingGitDependency() ? &dependencyOptions : nullptr;
}

std::filesystem::path resolveGitDependency(const SunConfig& consumer,
                                           const std::string& name) {
  /** Shortens filesystem operations within the dependency build. */
  namespace fs = std::filesystem;
  const auto& dep = consumer.dependencies.at(name);
  const auto repository = dep.git + "\n" + dep.version;
  const bool direct = !dep.path.empty();
  auto selection = repository + (direct ? "\nsource\n" + dep.path
                                        : "\nconfig\n" + dep.config);
  for (const auto& [id, entries] : dep.gitPackages) {
    selection += "\npackage\n" + id;
    for (const auto& entry : entries) {
      selection += "\nentrypoint\n" + entry;
    }
  }
  DependencyBuildGuard guard(selection);
  ConfigEntrypoint source;
  source.git = dep.git;
  source.version = dep.version;
  source.path = direct ? dep.path : dep.config;
  source.type = ConfigEntrypoint::Type::Library;
  const bool refresh =
      refreshDependencies && !refreshedDependencies.count(repository);
  const fs::path inputFile = resolveGitEntrypoint(source, refresh);
  if (refresh) {
    refreshedDependencies.insert(repository);
  }
  // Configured repositories share builds across consumers; direct sources
  // still inherit the importing project's configuration.
  std::string buildIdentity = selection + "\n" + inputFile.string() + "\n" +
                              (direct ? consumer.configDir.string() : "");
  const auto build = consumer.pathVariables.find("SUN_BUILD");
  const auto consumerOutput = build == consumer.pathVariables.end()
                                  ? fs::path{}
                                  : fs::path(build->second).lexically_normal();
  std::vector<std::string> consumerPaths;
  for (const auto& path : consumer.sunPath) {
    auto expanded = ManifestProcessor::expandPathVariables(path, &consumer);
    // Inherit explicit search paths, excluding the automatic output directory.
    if (!direct && !consumerOutput.empty() &&
        fs::path(expanded).lexically_normal() == consumerOutput) {
      continue;
    }
    buildIdentity += "\n" + expanded;
    consumerPaths.push_back(std::move(expanded));
  }
  const auto settings =
      std::string(dependencyOptions.debugInfo ? "debug" : "release") +
      (dependencyOptions.optimize ? "-optimized" : "-unoptimized");
  const auto output = cacheDirectory() / "builds" /
                      sun::moon_bundling::computeSha256Hex(buildIdentity) /
                      configTargetKey(consumer.targetTriple) / settings;
  auto checkout = inputFile.parent_path();
  while (!fs::is_directory(checkout / ".sun-git-source")) {
    if (checkout == checkout.parent_path()) {
      sun::support::logAndThrowError("missing Git checkout boundary");
    }
    checkout = checkout.parent_path();
  }
  if (!direct) {
    auto graph = loadConfigGraph(inputFile, consumer.targetTriple);
    graph.root = output;
    if (buildDepth && repositoryProducts.count(output)) {
      return repositoryProducts.at(output).at(name);
    }
    ConfigGraphScope graphScope(graph);
    std::map<std::string, SunConfig> providers;
    for (const auto& file : graph.projects) {
      auto relative = fs::weakly_canonical(file).lexically_relative(checkout);
      if (relative.empty() || *relative.begin() == "..") {
        sun::support::logAndThrowError(
            "dependency config escapes its checkout");
      }
      auto project = configureBuildPaths(
          graph, SunConfig::loadFile(file, consumer.targetTriple));
      if (dep.gitPackages.count(project.packageId)) {
        providers.emplace(project.packageId, std::move(project));
      }
    }
    for (const auto& [id, entries] : dep.gitPackages) {
      if (!providers.count(id)) {
        sun::support::logAndThrowError("Git repository has no package " + id);
      }
    }
    // The graph resolves sibling packages locally and shares their build roots.
    std::map<std::string, fs::path> installedProducts;
    for (auto& [id, project] : providers) {
      project.sunPath.insert(project.sunPath.begin(), consumerPaths.begin(),
                             consumerPaths.end());
      SourceConfigScope scope(project);
      const auto& names = dep.gitPackages.at(id);
      validateGitProducts(project, names, checkout);
      installedProducts[id] = buildConfigProducts(graph, project, names);
    }
    if (buildDepth) {
      repositoryProducts[output] = installedProducts;
    }
    return installedProducts.at(name);
  }
  auto config = consumer;
  config.sunPath = std::move(consumerPaths);
  SourceConfigScope configScope(config);
  const auto entrypoint = fs::weakly_canonical(inputFile);
  const auto relative = entrypoint.lexically_relative(checkout);
  if (relative.empty() || *relative.begin() == ".." ||
      !fs::is_regular_file(entrypoint)) {
    sun::support::logAndThrowError(
        "dependency library is missing or escapes its checkout");
  }
  fs::path filename = entrypoint.stem();
  if (filename.extension() != ".moon") {
    filename += ".moon";
  }
  if (filename.string().find('$') != std::string::npos) {
    sun::support::logAndThrowError(
        "dependency library output filename must be fixed");
  }
  fs::create_directories(output / "lib");
  auto options = dependencyOptions;
  options.targetTriple = consumer.targetTriple;
  sun::moon_bundling::MoonBuilder::build(entrypoint.string(),
                                         output / "lib" / filename, options);
  return output;
}
}  // namespace sun::driver
