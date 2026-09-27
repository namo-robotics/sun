// bundle_command.cpp — `sun --emit-moon`, which builds a .moon library.

#include "cli/bundle_command.h"

#include "cli/command_support.h"
#include "driver/git_source.h"
#include "llvm/Support/raw_ostream.h"
#include "support/error.h"
#include "support/terminal.h"

using sun::moon_bundling::MoonBuildOptions;

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/** Builds a compiled Moon library from an entrypoint and bundle settings. */
int buildMoonBundle(const std::string& entrypoint,
                    const std::filesystem::path& outputPath,
                    const MoonBuildOptions& buildOptions) {
  try {
    MoonBuildOptions announced = buildOptions;
    announced.onBuildStart = [&] {
      sun::support::messageStream("info")
          << "Creating moon: " << outputPath.string() << "\n";
    };
    auto report = sun::moon_bundling::MoonBuilder::build(entrypoint, outputPath,
                                                         announced);
    if (report.upToDate) {
      sun::support::messageStream("info")
          << "Up to date: " << outputPath.string() << "\n";
      return 0;
    }
    for (const auto& f : report.sunFiles) {
      sun::support::messageStream("info") << "  Including: " << f << "\n";
    }
    for (const auto& p : report.protoFiles) {
      sun::support::messageStream("info") << "  Including proto: " << p << "\n";
    }
    for (const auto& m : report.moonImports) {
      sun::support::messageStream("info")
          << "  Moon import: " << m.path << "\n";
    }
    sun::support::messageStream("info")
        << "Successfully created: " << outputPath.string() << "\n";
    return 0;
  } catch (const sun::support::SunError& e) {
    llvm::errs() << e.what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    sun::support::messageStream("error") << e.what() << "\n";
    return 1;
  }
}

/** Runs the bundle command and returns its process exit status. */
int runBundleCommand(const BuildRunOptions& options) {
  sun::driver::GitDependencyBuildScope dependencyScope(
      options.shared.debugInfo, options.shared.optimize, options.forceRebuild,
      options.refreshSources);
  const std::string& entrypoint = options.inputFiles[0];
  std::filesystem::path outputPath =
      options.outputFile.empty()
          ? sun::moon_bundling::MoonBuilder::defaultOutputPath(entrypoint)
          : std::filesystem::path(options.outputFile);

  MoonBuildOptions buildOptions;
  buildOptions.targetTriple = options.targetTriple;
  buildOptions.debugMode = options.shared.debugMode;
  buildOptions.debugInfo = options.shared.debugInfo;
  buildOptions.optimize = options.shared.optimize;
  buildOptions.dumpProtoSun = options.dumpProtoSun;
  buildOptions.extraMoons = options.shared.moonImports;
  buildOptions.forceRebuild = options.forceRebuild;

  return buildMoonBundle(entrypoint, outputPath, buildOptions);
}

}  // namespace sun::cli
