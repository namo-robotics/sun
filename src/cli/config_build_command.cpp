// config_build_command.cpp — `sun -c sun-config.json`, building a project.

#include "cli/config_build_command.h"

#include <filesystem>

#include "cli/bundle_command.h"
#include "cli/command_support.h"
#include "cli/compile_command.h"

namespace sun::cli {

namespace {

// Config-named outputs may sit in folders that do not exist yet (e.g.
// "build/stdlib"); create them so the artifacts have somewhere to land.
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

// The bundling flags that match a compile job's flags.
sun::MoonBuildOptions makeMoonBuildOptions(const CompileJob& job) {
  sun::MoonBuildOptions buildOptions;
  buildOptions.targetTriple = job.targetTriple;
  buildOptions.debugInfo = job.debugInfo;
  buildOptions.optimize = job.optimize;
  buildOptions.dumpProtoSun = job.dumpProtoSun;
  buildOptions.extraMoons = job.moonImports;
  return buildOptions;
}

// Build a library entrypoint: its .moon bundle, then its test binary. The
// bundle is the production artifact; the only executable a library yields is
// its test binary, and a library without tests yields none.
int buildLibrary(const CompileJob& job) {
  std::filesystem::path moonPath(job.outputFile);
  if (moonPath.extension() != ".moon") {
    moonPath += ".moon";
  }
  if (buildMoonBundle(job.inputFiles[0], moonPath, makeMoonBuildOptions(job),
                      job.depfile) != 0) {
    return 1;
  }
  if (job.noTest) {
    return 0;
  }
  try {
    return compileTestBinary(job);
  } catch (const SunError& e) {
    if (isNoTestsError(e)) {
      return 0;
    }
    return reportSunError(e);
  } catch (const std::exception& e) {
    return reportUnexpectedError(e);
  }
}

// Build every entrypoint the config declares, stopping at the first failure.
int buildEntrypoints(const sun::SunConfig& config, const CompileJob& base) {
  for (const auto& entry : config.entrypoints) {
    CompileJob job = base;
    job.inputFiles = {entry.path};
    job.outputFile = entry.outputName.empty() ? deriveOutputName(entry.path)
                                              : entry.outputName;
    job.testBinaryName = entry.testBinaryName;
    createOutputFolders(job);

    int exitCode = entry.type == sun::ConfigEntrypoint::Type::Library
                       ? buildLibrary(job)
                       : compileEntrypoint(job);
    if (exitCode != 0) {
      return 1;
    }
  }
  return 0;
}

}  // namespace

int runConfigBuildCommand(const BuildRunOptions& options) {
  const std::string& configFile = options.inputFiles[0];
  sun::Depfile depfile;
  CompileJob base =
      makeCompileJob(options, options.depfilePath.empty() ? nullptr : &depfile);
  try {
    // The config names the outputs, so every artifact depends on it too
    depfile.addSharedInput(std::filesystem::absolute(configFile).string());
    sun::SunConfig config = loadConfigInput(configFile, options.targetTriple);
    return writeDepfileOnSuccess(buildEntrypoints(config, base), depfile,
                                 options.depfilePath);
  } catch (const SunError& e) {
    return reportSunError(e);
  }
}

}  // namespace sun::cli
