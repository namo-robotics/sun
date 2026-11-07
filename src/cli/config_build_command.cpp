// config_build_command.cpp — `sun -c sun-config.json`, building a project.

#include "cli/config_build_command.h"

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/StringSaver.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

#include "cli/bundle_command.h"
#include "cli/command_support.h"
#include "cli/compile_command.h"
#include "cli/option_parser.h"
#include "driver/build_graph.h"
#include "driver/git_source.h"
#include "driver/manifest_processor.h"
#include "driver/package.h"
#include "support/display_path.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/**
 * The bundling flags that match a compile job's flags.
 */
sun::moon_bundling::MoonBuildOptions makeMoonBuildOptions(
    const CompileJob& job) {
  sun::moon_bundling::MoonBuildOptions buildOptions;
  buildOptions.targetTriple = job.targetTriple;
  buildOptions.debugMode = job.debugMode;
  buildOptions.debugInfo = job.debugInfo;
  buildOptions.optimize = job.optimize;
  buildOptions.dumpProtoSun = job.dumpProtoSun;
  buildOptions.extraMoons = job.moonImports;
  buildOptions.forceRebuild = job.forceRebuild;
  buildOptions.versionName = job.versionName;
  return buildOptions;
}

/**
 * Build a library entrypoint: its .moon bundle, then its test binary. The
 * bundle is the production artifact; the only executable a library yields is
 * its test binary, and a library without tests yields none.
 */
int buildLibrary(const CompileJob& job) {
  std::filesystem::path moonPath(job.outputFile);
  if (moonPath.extension() != ".moon") {
    moonPath += ".moon";
  }
  if (buildMoonBundle(job.inputFiles[0], moonPath, makeMoonBuildOptions(job)) !=
      0) {
    return 1;
  }
  if (job.noTest) {
    return 0;
  }
  try {
    return compileTestBinary(job);
  } catch (const sun::support::SunError& e) {
    if (isNoTestsError(e)) {
      return 0;
    }
    return reportSunError(e);
  } catch (const std::exception& e) {
    return reportUnexpectedError(e);
  }
}

/** Runs ordered custom scripts with a snapshot of already resolved
 * dependencies. */
void runCustomBuild(const BuildRunOptions& options,
                    const sun::driver::SunConfig& config,
                    const sun::driver::ConfigEntrypoint& entry,
                    const std::filesystem::path& output) {
  namespace fs = std::filesystem;
  auto settings = options;
  if (sun::driver::configTargetKey(options.targetTriple) !=
      sun::driver::configTargetKey(config.targetTriple)) {
    settings.linkOptions = {};
    settings.shared.libPaths.clear();
    settings.shared.moonImports.clear();
    settings.staticRequested = false;
    settings.dynamicRequested = false;
  }
  llvm::json::Object dependencies, hostDependencies, hostErrors, variables;
  const bool native =
      sun::driver::configTargetKey(config.targetTriple) ==
      sun::driver::configTargetKey(llvm::sys::getDefaultTargetTriple());
  for (const auto& [name, descriptor] : config.dependencies) {
    auto root = sun::driver::resolveConfigDependency(config, name).string();
    dependencies[name] = root;
    if (native) {
      hostDependencies[name] = root;
    } else if (!descriptor.gitPackages.empty() ||
               std::any_of(
                   descriptor.selections.begin(), descriptor.selections.end(),
                   [](const auto& selected) {
                     return selected.package || !selected.binary.empty();
                   })) {
      try {
        auto host = sun::driver::SunConfig::loadFile(
            config.configFile, llvm::sys::getDefaultTargetTriple());
        hostDependencies[name] =
            sun::driver::resolveConfigDependency(host, name).string();
      } catch (const sun::support::SunError& error) {
        // Target-only distributions need not provide tools for the build host.
        hostErrors[name] = std::string(error.what());
      }
    }
  }
  for (const auto& [key, value] : config.pathVariables) {
    variables[key] = value;
  }
  llvm::json::Array paths, flags;
  for (const auto& path : config.sunPath) {
    paths.push_back(path);
  }
  if (settings.shared.debugInfo) {
    flags.push_back("-g");
  }
  if (!settings.shared.optimize) {
    flags.push_back("-O0");
  }
  if (settings.shared.debugMode) {
    flags.push_back("--debug");
  }
  if (settings.noTest) {
    flags.push_back("--no-test");
  }
  if (settings.forceRebuild) {
    flags.push_back("--force-rebuild");
  }
  if (settings.dynamicRequested) {
    flags.push_back("--dynamic");
  }
  if (settings.staticRequested) {
    flags.push_back("--static");
  }
  if (!settings.versionName.empty()) {
    flags.push_back("--version-name");
    flags.push_back(settings.versionName);
  }
  if (!settings.linkOptions.sysroot.empty()) {
    flags.push_back("--sysroot");
    flags.push_back(settings.linkOptions.sysroot);
  }
  for (const auto& path : settings.linkOptions.searchPaths) {
    flags.push_back("-L" + fs::absolute(path).string());
  }
  for (const auto& library : settings.linkOptions.libraries) {
    flags.push_back("-l" + library);
  }
  for (const auto& path : settings.shared.libPaths) {
    flags.push_back("--lib-path");
    flags.push_back(fs::absolute(path).string());
  }
  for (const auto& [name, value] : settings.shared.pathVariables) {
    flags.push_back("--path-var");
    flags.push_back(name + "=" + value);
  }
  for (const auto& moon : settings.shared.moonImports) {
    std::string spec = fs::absolute(moon.path).string();
    bool first = true;
    for (const auto& [from, to] : moon.moduleRemap) {
      spec += (first ? ":" : ",") + from + "=" + to;
      first = false;
    }
    flags.push_back("--moon");
    flags.push_back(spec);
  }
  const auto source =
      sun::driver::ManifestProcessor::expandPathVariables(entry.path, &config);
  const auto contextPath = fs::path(config.pathVariables.at("SUN_BUILD")) /
                           ".contexts" / (entry.name + ".json");
  fs::create_directories(contextPath.parent_path());
  std::error_code error;
  llvm::raw_fd_ostream file(contextPath.string(), error);
  if (error) {
    sun::support::logAndThrowError("cannot write build context: " +
                                   error.message());
  }
  file << llvm::json::Value(llvm::json::Object{
      {"version", 1},
      {"config", config.configFile.string()},
      {"display_root", sun::support::displayPathBase().string()},
      {"target", config.targetTriple},
      {"source", source},
      {"output", output.string()},
      {"tests", entry.testBinaryName},
      {"library", entry.type == sun::driver::ConfigEntrypoint::Type::Library},
      {"export", entry.exported},
      {"flags", std::move(flags)},
      {"variables", std::move(variables)},
      {"sun_path", std::move(paths)},
      {"dependencies", std::move(dependencies)},
      {"host_dependencies", std::move(hostDependencies)},
      {"host_errors", std::move(hostErrors)}});
  file.close();
  if (file.has_error()) {
    sun::support::logAndThrowError("cannot save build context");
  }
  /** Restores process state after the sequential script invocation. */
  struct Environment {
    fs::path previousDirectory = fs::current_path();
    std::map<std::string, std::optional<std::string>> previous;
    /** Sets one inherited build variable while retaining its previous value. */
    void set(const std::string& name, const std::string& value) {
      const char* old = std::getenv(name.c_str());
      previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
      setenv(name.c_str(), value.c_str(), 1);
    }
    /** Restores the caller's environment even when a script fails. */
    ~Environment() {
      std::error_code ignored;
      fs::current_path(previousDirectory, ignored);
      for (const auto& [name, value] : previous) {
        if (value) {
          setenv(name.c_str(), value->c_str(), 1);
        } else {
          unsetenv(name.c_str());
        }
      }
    }
  } environment;
  environment.set("SUN_BUILD_CONTEXT", contextPath.string());
  environment.set(
      "SUN_EXECUTABLE",
      llvm::sys::fs::getMainExecutable(
          nullptr, reinterpret_cast<void*>(&configureProductCompiler)));
  environment.set("SUN_SOURCE", source);
  environment.set("SUN_OUTPUT", output.string());
  environment.set("SUN_TEST_OUTPUT", entry.testBinaryName);
  environment.set("SUN_TARGET", config.targetTriple);
  environment.set("SUN_BUILD", config.pathVariables.at("SUN_BUILD"));
  environment.set("SUN_INSTALL", config.pathVariables.at("SUN_INSTALL"));
  environment.set("SUN_JOBS", config.pathVariables.at("SUN_JOBS"));
  fs::current_path(config.configDir);
  for (const auto& script : entry.build) {
    std::string message;
    if (llvm::sys::ExecuteAndWait(script, {script}, std::nullopt, {}, 0, 0,
                                  &message) != 0) {
      sun::support::logAndThrowError("build script failed: " + script +
                                     (message.empty() ? "" : ": " + message));
    }
  }
}

}  // namespace

/** Runs the config build command and returns its process exit status. */
int runConfigBuildCommand(const BuildRunOptions& options) {
  sun::support::DisplayPathScope displayPaths(
      std::filesystem::absolute(options.inputFiles.at(0)).parent_path());
  sun::driver::GitDependencyBuildScope dependencyScope(
      options.shared.debugInfo, options.shared.optimize, options.forceRebuild,
      options.refreshSources);
  try {
    auto graph = sun::driver::loadConfigGraph(options.inputFiles.at(0),
                                              options.targetTriple);
    sun::driver::ConfigGraphScope graphScope(graph);
    configureProductCompiler(options);
    bool matched = false;
    for (const auto& file : graph.projects) {
      auto config =
          sun::driver::SunConfig::loadFile(file, options.targetTriple);
      if (!options.buildPackage.empty() &&
          config.packageId != options.buildPackage) {
        continue;
      }
      if (config.entrypoints.empty()) {
        continue;
      }
      matched = true;
      auto names = options.buildEntrypoint.empty()
                       ? sun::driver::defaultConfigEntrypoints(config)
                       : std::vector<std::string>{options.buildEntrypoint};
      sun::driver::buildConfigProducts(graph, config, names);
      if (options.buildEntrypoint.empty()) {
        sun::driver::packageConfigProducts(graph, std::move(config), names);
      }
    }
    if (!matched) {
      sun::support::logAndThrowError(
          "no matching build products in config graph");
    }
    return 0;
  } catch (const sun::support::SunError& e) {
    return reportSunError(e);
  } catch (const std::exception& e) {
    return reportUnexpectedError(e);
  }
}

void configureProductCompiler(const BuildRunOptions& options) {
  sun::driver::setBuildConfiguration(
      std::string(options.shared.debugInfo ? "debug" : "release") +
          (options.shared.optimize ? "" : "-unoptimized"),
      options.forceRebuild);
  sun::driver::setProductCompiler([options](
                                      const sun::driver::SunConfig& config,
                                      const sun::driver::ConfigEntrypoint&
                                          entry,
                                      const std::filesystem::path& output) {
    /** Restores the caller's source configuration after compiling a dependency.
     */
    struct ConfigGuard {
      const sun::driver::SunConfig* previous;
      /** Applies the producer's settings to all source compilation. */
      explicit ConfigGuard(const sun::driver::SunConfig& config)
          : previous(
                sun::driver::ManifestProcessor::exchangeSourceConfig(&config)) {
      }
      /** Restores the outer consumer even if compilation fails. */
      ~ConfigGuard() {
        sun::driver::ManifestProcessor::exchangeSourceConfig(previous);
      }
    } guard(config);
    auto selectedOptions = options;
    if (const auto* dependency = sun::driver::gitDependencyBuildOptions()) {
      selectedOptions.shared.debugInfo = dependency->debugInfo;
      selectedOptions.shared.optimize = dependency->optimize;
      selectedOptions.forceRebuild = dependency->forceRebuild;
    }
    selectedOptions.noTest =
        options.noTest || sun::driver::isCurrentlyBuildingDependency();
    if (!entry.build.empty()) {
      runCustomBuild(selectedOptions, config, entry, output);
      return;
    }
    if (!entry.compileArgs.empty() || !entry.pkgConfig.empty()) {
      std::vector<std::string> args;
      for (const auto& arg : entry.compileArgs) {
        args.push_back(
            sun::driver::ManifestProcessor::expandPathVariables(arg, &config));
      }
      if (!entry.pkgConfig.empty()) {
        auto program = llvm::sys::findProgramByName("pkg-config");
        if (!program) {
          sun::support::logAndThrowError("pkg-config is required by " +
                                         entry.path);
        }
        const auto flagsPath = output.string() + ".link-flags";
        std::vector<llvm::StringRef> query{*program, "--libs"};
        for (const auto& package : entry.pkgConfig) {
          query.push_back(package);
        }
        std::optional<llvm::StringRef> redirects[] = {
            llvm::StringRef(""), llvm::StringRef(flagsPath), std::nullopt};
        if (llvm::sys::ExecuteAndWait(*program, query, std::nullopt,
                                      redirects) != 0) {
          sun::support::logAndThrowError("pkg-config failed for " + entry.path);
        }
        auto contents = llvm::MemoryBuffer::getFile(flagsPath);
        if (!contents) {
          sun::support::logAndThrowError("cannot read pkg-config flags");
        }
        llvm::BumpPtrAllocator allocator;
        llvm::StringSaver saver(allocator);
        llvm::SmallVector<const char*> flags;
        llvm::cl::TokenizeGNUCommandLine((*contents)->getBuffer(), saver,
                                         flags);
        for (const auto* flag : flags) {
          args.emplace_back(flag);
        }
      }
      args.insert(args.end(), {"-c", entry.path, "--no-test"});
      if (!config.targetTriple.empty()) {
        args.insert(args.end(), {"--target", config.targetTriple});
      }
      const bool forceRebuild = selectedOptions.forceRebuild;
      if (auto error =
              parseBuildRunArguments("sun build", args, selectedOptions)) {
        reportEarlyExit(*error);
        sun::support::logAndThrowError("invalid compile_args for " +
                                       entry.path);
      }
      selectedOptions.forceRebuild = forceRebuild;
    }
    auto job = makeCompileJob(selectedOptions);
    job.inputFiles = {
        entry.git.empty()
            ? sun::driver::ManifestProcessor::expandPathVariables(entry.path,
                                                                  &config)
            : sun::driver::resolveGitEntrypoint(entry, options.refreshSources)};
    job.outputFile = output.string();
    job.targetTriple = config.targetTriple;
    if (sun::driver::configTargetKey(options.targetTriple) !=
        sun::driver::configTargetKey(config.targetTriple)) {
      job.baseLinkOpts = {};
      job.moonImports.clear();
    }
    job.baseLinkOpts.targetTriple = config.targetTriple;
    if (sun::driver::effectiveLinkTriple(config.targetTriple).isOSDarwin()) {
      job.baseLinkOpts.staticLink = false;
    }
    for (const auto& argument : entry.linkArgs) {
      job.baseLinkOpts.arguments.push_back(
          sun::driver::ManifestProcessor::expandPathVariables(argument,
                                                              &config));
    }
    job.noTest = options.noTest || sun::driver::isCurrentlyBuildingDependency();
    job.testBinaryName = sun::driver::ManifestProcessor::expandPathVariables(
        entry.testBinaryName, &config);
    job.requireProductionArtifact = entry.exported;
    const bool library =
        entry.type == sun::driver::ConfigEntrypoint::Type::Library;
    const auto status = library ? buildLibrary(job) : compileEntrypoint(job);
    if (status != 0) {
      sun::support::logAndThrowError("config build failed: " +
                                     sun::support::displayPath(entry.path));
    }
  });
}

/** Identifies the operation applied to a selected config graph. */
enum class ConfigAction { Build, Resolve, Describe };

/** Parses config operations through the same root and project selection. */
static int runConfigCommand(const std::vector<std::string>& args,
                            ConfigAction action) {
  try {
    std::vector<std::string> compilerArgs;
    std::string package, entrypoint, configFile, dependency, field;
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "--entrypoint" || args[i] == "--package" ||
          args[i] == "--config" || args[i] == "--dependency" ||
          (action == ConfigAction::Describe && args[i] == "--field")) {
        const auto flag = args[i];
        if (++i == args.size()) {
          sun::support::logAndThrowError(flag + " requires a value");
        }
        if (flag == "--entrypoint") {
          entrypoint = args[i];
        }
        if (flag == "--package") {
          package = args[i];
        }
        if (flag == "--config") {
          configFile = args[i];
        }
        if (flag == "--dependency") {
          dependency = args[i];
        }
        if (flag == "--field") {
          field = args[i];
          if (field != "build_root" && field != "install_root" &&
              field != "package_id") {
            sun::support::logAndThrowError(
                "--field expects build_root, install_root, or package_id");
          }
        }
      } else {
        compilerArgs.push_back(args[i]);
      }
    }
    if (configFile.empty()) {
      auto directory = std::filesystem::current_path();
      for (;;) {
        auto candidate = directory / "sun-config.json";
        if (std::filesystem::is_regular_file(candidate)) {
          configFile = candidate.string();
          break;
        }
        if (directory == directory.parent_path()) {
          sun::support::logAndThrowError("no sun-config.json found");
        }
        directory = directory.parent_path();
      }
    }
    compilerArgs.insert(compilerArgs.end(), {"-c", configFile});
    BuildRunOptions options;
    if (auto early =
            parseBuildRunArguments("sun build", compilerArgs, options)) {
      return reportEarlyExit(*early);
    }
    options.buildPackage = package;
    options.buildEntrypoint = entrypoint;
    applyBuildRunSettings(options);
    if (action == ConfigAction::Build) {
      return runConfigBuildCommand(options);
    }
    if (action == ConfigAction::Resolve && dependency.empty()) {
      sun::support::logAndThrowError("sun resolve requires --dependency");
    }
    auto graph = sun::driver::loadConfigGraph(configFile, options.targetTriple);
    sun::driver::ConfigGraphScope scope(graph);
    configureProductCompiler(options);
    for (const auto& file : graph.projects) {
      auto config =
          sun::driver::SunConfig::loadFile(file, options.targetTriple);
      if ((!package.empty() && config.packageId != package) ||
          (package.empty() && file != graph.projects.front())) {
        continue;
      }
      config = sun::driver::configureBuildPaths(graph, std::move(config));
      if (action == ConfigAction::Describe) {
        if (!field.empty()) {
          const auto& value =
              field == "package_id"
                  ? config.packageId
                  : config.pathVariables.at(
                        field == "build_root" ? "SUN_BUILD" : "SUN_INSTALL");
          llvm::outs() << value << "\n";
          return 0;
        }
        llvm::json::Array entries;
        for (const auto& entry : config.entrypoints) {
          const auto path = sun::driver::ManifestProcessor::expandPathVariables(
              entry.path, &config);
          const auto output =
              entry.outputName.empty()
                  ? (std::filesystem::path(
                         config.pathVariables.at("SUN_BUILD")) /
                     std::filesystem::path(path).stem())
                        .string()
                  : sun::driver::ManifestProcessor::expandPathVariables(
                        entry.outputName, &config);
          const auto tests =
              entry.testBinaryName.empty()
                  ? output + "_test"
                  : sun::driver::ManifestProcessor::expandPathVariables(
                        entry.testBinaryName, &config);
          entries.push_back(llvm::json::Object{
              {"name", entry.name},
              {"path", path},
              {"output_name", output},
              {"test_binary_name", tests},
              {"type",
               entry.type == sun::driver::ConfigEntrypoint::Type::Library
                   ? "library"
                   : "binary"}});
        }
        llvm::outs()
            << llvm::json::Value(llvm::json::Object{
                   {"package_id", config.packageId},
                   {"build_root", config.pathVariables.at("SUN_BUILD")},
                   {"install_root", config.pathVariables.at("SUN_INSTALL")},
                   {"entrypoints", std::move(entries)}})
            << "\n";
        return 0;
      }
      const auto root =
          sun::driver::resolveConfigDependency(config, dependency);
      llvm::json::Value result(llvm::json::Object{
          {"package", dependency},
          {"root", root.string()},
          {"target", sun::driver::configTargetKey(config.targetTriple)}});
      llvm::outs() << result << "\n";
      return 0;
    }
    sun::support::logAndThrowError("consumer package not found: " + package);
  } catch (const sun::support::SunError& error) {
    return reportSunError(error);
  } catch (const std::exception& error) {
    return reportUnexpectedError(error);
  }
}

int runBuildCommand(const std::vector<std::string>& args) {
  return runConfigCommand(args, ConfigAction::Build);
}

int runResolveCommand(const std::vector<std::string>& args) {
  return runConfigCommand(args, ConfigAction::Resolve);
}

int runDescribeCommand(const std::vector<std::string>& args) {
  return runConfigCommand(args, ConfigAction::Describe);
}

int runDependencyPathCommand(const std::vector<std::string>& args) {
  try {
    const bool host = args.size() == 2 && args[0] == "--host";
    if (args.size() != 1 && !host) {
      sun::support::logAndThrowError(
          "usage: sun dependency-path [--host] <package>");
    }
    auto context = sun::driver::readBuildContext();
    auto* roots =
        context.getObject(host ? "host_dependencies" : "dependencies");
    auto path = roots ? roots->getString(args.back()) : std::nullopt;
    if (!path && host) {
      if (auto* errors = context.getObject("host_errors")) {
        if (auto error = errors->getString(args.back())) {
          sun::support::logAndThrowError("host dependency unavailable: " +
                                         error->str());
        }
      }
    }
    if (!path) {
      sun::support::logAndThrowError(
          "dependency is absent from build context: " + args.back());
    }
    llvm::outs() << *path << "\n";
    return 0;
  } catch (const sun::support::SunError& e) {
    return reportSunError(e);
  } catch (const std::exception& e) {
    return reportUnexpectedError(e);
  }
}

int runBuildCompileCommand(const std::vector<std::string>& args) {
  try {
    auto context = sun::driver::readBuildContext();
    sun::support::DisplayPathScope displayPaths(
        std::string(context.getString("display_root").value_or("")));
    auto config = sun::driver::buildContextConfig();
    auto* inherited = context.getArray("flags");
    auto source = context.getString("source"),
         output = context.getString("output"),
         tests = context.getString("tests");
    if (!inherited || !source || !output || !tests) {
      sun::support::logAndThrowError("incomplete compiler build context");
    }
    std::vector<std::string> flags, linkArgs;
    for (const auto& flag : *inherited) {
      if (!flag.getAsString()) {
        sun::support::logAndThrowError("invalid context compiler flag");
      }
      flags.push_back(flag.getAsString()->str());
    }
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "--link-arg") {
        if (++i == args.size()) {
          sun::support::logAndThrowError("--link-arg requires a value");
        }
        linkArgs.push_back(args[i]);
      } else {
        flags.push_back(args[i]);
      }
    }
    flags.insert(flags.end(), {"-c", source->str(), "-o", output->str(),
                               "--target", config.targetTriple});
    BuildRunOptions options;
    if (auto early =
            parseBuildRunArguments("sun build-compile", flags, options)) {
      return reportEarlyExit(*early);
    }
    if (options.inputFiles.size() != 1 || options.configInput ||
        options.emitObjOnly || options.emitMoon) {
      sun::support::logAndThrowError(
          "build-compile must produce the configured artifact");
    }
    applyBuildRunSettings(options);
    auto job = makeCompileJob(options);
    job.testBinaryName = tests->str();
    job.requireProductionArtifact =
        context.getBoolean("export").value_or(false);
    job.baseLinkOpts.arguments = std::move(linkArgs);
    if (sun::driver::effectiveLinkTriple(config.targetTriple).isOSDarwin()) {
      job.baseLinkOpts.staticLink = false;
    }
    return context.getBoolean("library").value_or(false)
               ? buildLibrary(job)
               : compileEntrypoint(job);
  } catch (const sun::support::SunError& e) {
    return reportSunError(e);
  } catch (const std::exception& e) {
    return reportUnexpectedError(e);
  }
}

}  // namespace sun::cli
