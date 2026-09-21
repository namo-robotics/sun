// config_build_command.cpp — `sun -c sun-config.json`, building a project.

#include "cli/config_build_command.h"

#include <filesystem>

#include "cli/bundle_command.h"
#include "cli/command_support.h"
#include "cli/compile_command.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/**
 * Config-named outputs may sit in folders that do not exist yet (e.g.
 * "build/stdlib"); create them so the artifacts have somewhere to land.
 */
void createOutputFolders(const CompileJob& job) {
  std::error_code ec;
  for (const std::string& artifact : {job.outputFile, job.testBinaryName}) {
    std::filesystem::path parent =
        std::filesystem::path(artifact).parent_path();
    if (!artifact.empty() && !parent.empty()) {
      std::filesystem::create_directories(parent, ec);
    }
  }
}

/**
 * The bundling flags that match a compile job's flags.
 */
sun::moon_bundling::MoonBuildOptions makeMoonBuildOptions(
    const CompileJob& job) {
  sun::moon_bundling::MoonBuildOptions buildOptions;
  buildOptions.targetTriple = job.targetTriple;
  buildOptions.debugInfo = job.debugInfo;
  buildOptions.optimize = job.optimize;
  buildOptions.dumpProtoSun = job.dumpProtoSun;
  buildOptions.extraMoons = job.moonImports;
  buildOptions.forceRebuild = job.forceRebuild;
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

/**
 * Build every entrypoint the config declares, stopping at the first failure.
 */
int buildEntrypoints(const sun::driver::SunConfig& config,
                     const CompileJob& base) {
  for (const auto& entry : config.entrypoints) {
    CompileJob job = base;
    job.inputFiles = {entry.path};
    job.outputFile = entry.outputName.empty() ? deriveOutputName(entry.path)
                                              : entry.outputName;
    job.testBinaryName = entry.testBinaryName;
    createOutputFolders(job);

    int exitCode = entry.type == sun::driver::ConfigEntrypoint::Type::Library
                       ? buildLibrary(job)
                       : compileEntrypoint(job);
    if (exitCode != 0) {
      return 1;
    }
  }
  return 0;
}

}  // namespace

/** Runs the config build command and returns its process exit status. */
int runConfigBuildCommand(const BuildRunOptions& options) {
  const std::string& configFile = options.inputFiles[0];
  CompileJob base = makeCompileJob(options);
  try {
    sun::driver::SunConfig config =
        loadConfigInput(configFile, options.targetTriple);
    return buildEntrypoints(config, base);
  } catch (const sun::support::SunError& e) {
    return reportSunError(e);
  }
}

}  // namespace sun::cli
