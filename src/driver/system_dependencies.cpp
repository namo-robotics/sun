// Checks native requirements and invokes package managers only on explicit request.
#include "driver/system_dependencies.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/TargetParser/Host.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <set>

#include "driver/build_graph.h"
#include "driver/package.h"
#include "driver/git_source.h"
#include "moon_bundling/moon_cache.h"
#include "support/error.h"
#include "support/target_os.h"

/** Checks and explicitly installs dependencies supplied by the host system. */
namespace sun::driver {
/** Keeps process execution and package-manager policy private. */
namespace {

/** Captures a tool's status and standard output without invoking a shell. */
struct ToolResult {
  int status;
  std::string output;
};

/** Selects whether a subprocess needs captured, discarded, or interactive output. */
enum class ToolOutput { Capture, Silent, Inherit };

/** Runs a tool with argument boundaries preserved and explicit output handling. */
ToolResult run(const SunConfig& config, std::vector<std::string> args,
               ToolOutput output = ToolOutput::Capture) {
  auto tool = llvm::sys::findProgramByName(args.front());
  if (!tool) {
    return {-1, ""};
  }
  args.front() = *tool;
  std::vector<llvm::StringRef> refs(args.begin(), args.end());
  if (output == ToolOutput::Inherit) {
    llvm::outs().flush();
    return {llvm::sys::ExecuteAndWait(*tool, refs), ""};
  }
  if (output == ToolOutput::Silent) {
    std::optional<llvm::StringRef> redirects[] = {
        llvm::StringRef(""), llvm::StringRef(""), llvm::StringRef("")};
    return {llvm::sys::ExecuteAndWait(*tool, refs, std::nullopt, redirects), ""};
  }
  const auto directory = configWorkspaceRoot(config) / "tmp";
  std::filesystem::create_directories(directory);
  llvm::SmallString<256> file;
  int fd;
  if (auto error = llvm::sys::fs::createUniqueFile(
          (directory / "sun-dep-%%%%%%").string(), fd, file)) {
    sun::support::logAndThrowError("cannot create dependency check output: " + error.message());
  }
  close(fd);
  /** Removes captured output even when a tool or subsequent read fails. */
  struct Cleanup {
    std::filesystem::path path;
    /** Removes the temporary output file. */
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } cleanup{file.str().str()};
  std::optional<llvm::StringRef> redirects[] = {
      llvm::StringRef(""), file.str(), llvm::StringRef("")};
  int status = llvm::sys::ExecuteAndWait(*tool, refs, std::nullopt, redirects);
  auto buffer = llvm::MemoryBuffer::getFile(file);
  return {status, buffer ? buffer.get()->getBuffer().trim().str() : ""};
}

/** Formats a check for diagnostics and pkg-config's single-argument constraint. */
std::string requirement(const ConfigSystemDependency& dep) {
  if (!dep.check) {
    std::string packages;
    for (const auto& name : dep.apt) packages += name + " ";
    for (const auto& name : dep.brew) packages += name + " ";
    return packages;
  }
  const auto& check = *dep.check;
  if (check.version.empty()) {
    return check.name;
  }
  const bool comparison = check.version.front() == '<' ||
                          check.version.front() == '>' || check.version.front() == '=';
  return check.name + (comparison ? " " : " = ") + check.version;
}

/** Runs a declared check; an absent check cannot establish availability. */
bool present(const SunConfig& config, const ConfigSystemDependency& dep) {
  if (!dep.check) {
    return false;
  }
  if (dep.check->type == ConfigDependencyCheck::Type::Executable) {
    return bool(llvm::sys::findProgramByName(dep.check->name));
  }
  return run(config, {"pkg-config", "--exists", requirement(dep)},
             ToolOutput::Silent).status == 0;
}

/** Refuses to use a host package manager to satisfy another target's libraries. */
void requireNativeTarget(const SunConfig& config) {
  if (configTargetKey(config.targetTriple) != configTargetKey(llvm::sys::getDefaultTargetTriple())) {
    sun::support::logAndThrowError("system dependency installation requires the host target; provide target libraries through a sysroot for " + config.targetTriple);
  }
}

/** Prints a readable command plan; execution still uses an argument vector. */
void printCommand(const std::vector<std::string>& args) {
  auto& stream = sun::support::messageStream("info");
  stream << "Install:";
  for (const auto& arg : args) stream << " '" << arg << "'";
  stream << "\n";
  stream.flush();
}

/** Builds a host package-manager command without automatically accepting prompts. */
std::vector<std::string> installCommand(const std::vector<std::string>& apt,
                                       const std::vector<std::string>& brew,
                                       bool yes) {
  const auto host = sun::support::resolvedTargetTriple(llvm::sys::getDefaultTargetTriple());
  std::vector<std::string> command;
  if (host.isOSLinux() && !apt.empty() && llvm::sys::findProgramByName("apt-get")) {
    if (geteuid() != 0) {
      command.push_back("sudo");
    }
    command.insert(command.end(), {"apt-get", "install"});
    if (yes) {
      command.push_back("-y");
    }
    command.push_back("--");
    command.insert(command.end(), apt.begin(), apt.end());
  } else if (host.isOSDarwin() && !brew.empty() && llvm::sys::findProgramByName("brew")) {
    command = {"brew", "install", "--"};
    command.insert(command.end(), brew.begin(), brew.end());
  } else {
    sun::support::logAndThrowError("no supported package-manager mapping for this host");
  }
  return command;
}

/** Returns the stable location of a verified Debian artifact. */
std::filesystem::path debCacheFile(const SunConfig& config,
                                  const ConfigSystemDependency& dep) {
  return std::filesystem::absolute(configWorkspaceRoot(config) /
      ".sun/cache/debs" / dep.hash / "package.deb");
}

/** Caches a pinned Debian artifact without running package installation scripts. */
std::filesystem::path debFile(const SunConfig& config,
                             const ConfigSystemDependency& dep) {
  const auto cached = debCacheFile(config, dep);
  if (std::filesystem::is_regular_file(cached) && packageFileHash(cached) == dep.hash) {
    return cached;
  }
  const auto source = dep.url.empty() ? std::filesystem::path(dep.path)
      : sun::moon_bundling::MoonCache::fetch(dep.url, dep.hash, cached.parent_path());
  if (packageFileHash(source) != dep.hash) {
    sun::support::logAndThrowError("Debian package checksum mismatch: " + source.string());
  }
  std::filesystem::create_directories(cached.parent_path());
  std::filesystem::copy_file(source, cached, std::filesystem::copy_options::overwrite_existing);
  if (packageFileHash(cached) != dep.hash) {
    sun::support::logAndThrowError("Debian package changed while caching: " + source.string());
  }
  return cached;
}

/** Checks a Debian package's identity, version, and architecture before installation. */
bool debInstalled(const SunConfig& config, const std::filesystem::path& file) {
  auto name = run(config, {"dpkg-deb", "--field", file.string(), "Package"});
  auto version = run(config, {"dpkg-deb", "--field", file.string(), "Version"});
  auto arch = run(config, {"dpkg-deb", "--field", file.string(), "Architecture"});
  auto host = run(config, {"dpkg", "--print-architecture"});
  if (name.status || version.status || arch.status || host.status || name.output.empty() || version.output.empty()) {
    sun::support::logAndThrowError("cannot inspect Debian package: " + file.string());
  }
  if (arch.output != "all" && arch.output != host.output) {
    sun::support::logAndThrowError("Debian package architecture does not match host: " + arch.output);
  }
  auto installed = run(config, {"dpkg-query", "--show", "--showformat=${Status}\n${Version}", name.output});
  return installed.status == 0 && installed.output == "install ok installed\n" + version.output;
}

/** Visits source configs and stages artifacts without invoking product compilation. */
class DependencyInstaller {
 public:
  /** Stores installation policy shared by all projects in one invocation. */
  DependencyInstaller(bool dryRun, bool yes) : dryRun_(dryRun), yes_(yes) {}

  /** Installs each project's requirements once, including transitive source configs. */
  void install(const ConfigGraph& graph, const SunConfig& config) {
    const auto key = std::filesystem::weakly_canonical(config.configFile).string() +
                     "\n" + configTargetKey(config.targetTriple);
    if (!visited_.insert(key).second) {
      return;
    }
    installSystemDependencies(config, dryRun_, yes_);
    for (const auto& [name, dep] : config.dependencies) {
      installDependency(graph, config, name, dep);
    }
  }

 private:
  bool dryRun_;
  bool yes_;
  std::set<std::string> visited_;

  /** Validates a provider's selected products and visits its requirements without builds. */
  bool installProvider(const ConfigGraph& graph, const SunConfig& consumer,
                       const std::string& name, const ConfigDependency& dep) {
    for (const auto& file : graph.projects) {
      auto provider = SunConfig::loadFile(file, consumer.targetTriple);
      if (provider.packageId != name) {
        continue;
      }
      findConfigEntrypoints(provider, dep.entrypoints.empty()
          ? defaultConfigEntrypoints(provider) : dep.entrypoints);
      install(graph, configureBuildPaths(graph, std::move(provider)));
      return true;
    }
    return false;
  }

  /** Prefers local providers, fetches Git sources, or installs prebuilt artifacts. */
  void installDependency(const ConfigGraph& graph, const SunConfig& config,
                         const std::string& name, const ConfigDependency& dep) {
    if (installProvider(graph, config, name, dep)) {
      return;
    }
    if (!dep.selections.empty()) {
      for (const auto& selection : dep.selections) {
        installDependency(graph, config, name, selection);
      }
      return;
    }
    auto& log = sun::support::messageStream("info");
    log << (dep.git.empty() ? "Installing prebuilt dependency " : "Fetching source dependency ")
        << name << (dryRun_ ? " (dry run)\n" : "\n");
    log.flush();
    if (dryRun_) {
      return;
    }
    if (dep.git.empty()) {
      auto single = config;
      single.dependencies[name] = dep;
      // Local providers were handled above; artifact resolution must never build one.
      ConfigGraph empty;
      empty.root = configWorkspaceRoot(config);
      ConfigGraphScope scope(empty);
      resolveConfigDependency(single, name);
      return;
    }
    ConfigEntrypoint source;
    source.type = ConfigEntrypoint::Type::Library;
    source.git = dep.git;
    source.version = dep.version;
    source.path = dep.path.empty() ? dep.config : dep.path;
    const std::filesystem::path input = resolveGitEntrypoint(source);
    if (!dep.path.empty()) {
      return;  // Direct-source dependencies do not use their repository's config.
    }
    auto checkout = input.parent_path();
    while (!std::filesystem::is_directory(checkout / ".sun-git-source")) {
      if (checkout == checkout.parent_path()) {
        sun::support::logAndThrowError("missing Git checkout boundary");
      }
      checkout = checkout.parent_path();
    }
    auto dependencyGraph = loadConfigGraph(input, config.targetTriple);
    for (const auto& file : dependencyGraph.projects) {
      const auto relative = std::filesystem::weakly_canonical(file).lexically_relative(checkout);
      if (relative.empty() || *relative.begin() == "..") {
        sun::support::logAndThrowError("dependency config escapes its checkout");
      }
    }
    if (installProvider(dependencyGraph, config, name, dep)) {
      return;
    }
    sun::support::logAndThrowError("Git repository has no package " + name);
  }
};
}  // namespace

void checkSystemDependencies(const SunConfig& config) {
  std::string missing;
  for (const auto& dep : config.systemDependencies) {
    if (dep.check) {
      // Cross checks must use an explicitly configured target pkg-config search path.
      if (dep.check->type == ConfigDependencyCheck::Type::PkgConfig &&
          configTargetKey(config.targetTriple) != configTargetKey(llvm::sys::getDefaultTargetTriple()) &&
          !std::getenv("PKG_CONFIG_LIBDIR")) {
        sun::support::logAndThrowError(
            "cross-target system dependencies require PKG_CONFIG_LIBDIR for the target sysroot");
      }
      if (!present(config, dep)) {
        missing += "\n  " + requirement(dep);
      }
    } else if (dep.type == "deb") {
      requireNativeTarget(config);
      // Ordinary builds never download or install Debian packages.
      const auto cached = debCacheFile(config, dep);
      if (!std::filesystem::is_regular_file(cached) ||
          packageFileHash(cached) != dep.hash || !debInstalled(config, cached)) {
        missing += "\n  Debian package " + (dep.url.empty() ? dep.path : dep.url);
      }
    }
  }
  if (!missing.empty()) {
    sun::support::logAndThrowError(
      "missing system dependencies for " + (config.packageId.empty() ? config.configDir.string() : config.packageId) +
      ":" + missing + "\nRun sun dep install --config " + config.configFile.string());
  }
}

void installSystemDependencies(const SunConfig& config, bool dryRun, bool yes) {
  auto& log = sun::support::messageStream("info");
  log << (dryRun ? "Checking" : "Installing") << " system dependencies for "
      << (config.packageId.empty() ? config.configFile.string() : config.packageId)
      << (config.systemDependencies.empty() ? ": none declared\n" : "\n");
  log.flush();
  for (const auto& dep : config.systemDependencies) {
    requireNativeTarget(config);
    if (dep.type == "system") {
      if (present(config, dep)) {
        continue;
      }
      auto command = installCommand(dep.apt, dep.brew, yes);
      printCommand(command);
      if (dryRun) {
        continue;
      }
      if (run(config, command, ToolOutput::Inherit).status != 0) {
        sun::support::logAndThrowError("system package installation failed: " + requirement(dep));
      }
      if (dep.check && !present(config, dep)) {
        sun::support::logAndThrowError("installed packages do not satisfy " + requirement(dep));
      }
    } else {
      if (!sun::support::resolvedTargetTriple(config.targetTriple).isOSLinux()) {
        sun::support::logAndThrowError("Debian packages require a Linux host with APT");
      }
      if (dryRun) {
        sun::support::messageStream("info") << "Verify SHA-256 " << dep.hash << " and install Debian package "
            << (dep.url.empty() ? dep.path : dep.url) << " through APT\n";
        continue;
      }
      auto file = debFile(config, dep);
      if (!debInstalled(config, file)) {
        auto command = installCommand({file.string()}, {}, yes);
        printCommand(command);
        if (run(config, command, ToolOutput::Inherit).status != 0 || !debInstalled(config, file)) {
          sun::support::logAndThrowError("Debian package installation failed: " + file.string());
        }
      }
      if (dep.check && !present(config, dep)) {
        sun::support::logAndThrowError("Debian package does not satisfy " + requirement(dep));
      }
    }
  }
}

void installConfigDependencies(const ConfigGraph& graph,
                               const std::vector<SunConfig>& configs,
                               bool dryRun, bool yes) {
  ConfigGraphScope scope(graph);
  DependencyInstaller installer(dryRun, yes);
  for (const auto& config : configs) {
    installer.install(graph, config);
  }
}

}
