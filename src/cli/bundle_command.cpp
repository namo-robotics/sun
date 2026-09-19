// bundle_command.cpp — `sun --emit-moon`, which builds a .moon library.

#include "cli/bundle_command.h"

#include "cli/command_support.h"
#include "llvm/Support/raw_ostream.h"
#include "support/error.h"

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
      llvm::outs() << "Creating moon: " << outputPath.string() << "\n";
    };
    auto report = sun::moon_bundling::MoonBuilder::build(entrypoint, outputPath,
                                                         announced);
    if (report.upToDate) {
      llvm::outs() << "Up to date: " << outputPath.string() << "\n";
      return 0;
    }
    for (const auto& f : report.sunFiles) {
      llvm::outs() << "  Including: " << f << "\n";
    }
    for (const auto& p : report.protoFiles) {
      llvm::outs() << "  Including proto: " << p << "\n";
    }
    for (const auto& m : report.moonImports) {
      llvm::outs() << "  Moon import: " << m.path << "\n";
    }
    llvm::outs() << "Successfully created: " << outputPath.string() << "\n";
    return 0;
  } catch (const sun::support::SunError& e) {
    // Unlike the other commands, bundling prefixes compile errors with
    // "Error: ". The difference is deliberate: it keeps the output people
    // and scripts already see.
    llvm::errs() << "Error: " << e.what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    llvm::errs() << "Error: " << e.what() << "\n";
    return 1;
  }
}

/** Runs the bundle command and returns its process exit status. */
int runBundleCommand(const BuildRunOptions& options) {
  const std::string& entrypoint = options.inputFiles[0];
  std::filesystem::path outputPath =
      options.outputFile.empty()
          ? sun::moon_bundling::MoonBuilder::defaultOutputPath(entrypoint)
          : std::filesystem::path(options.outputFile);

  MoonBuildOptions buildOptions;
  buildOptions.targetTriple = options.targetTriple;
  buildOptions.debugInfo = options.shared.debugInfo;
  buildOptions.optimize = options.shared.optimize;
  buildOptions.dumpProtoSun = options.dumpProtoSun;
  buildOptions.extraMoons = options.shared.moonImports;
  buildOptions.skipIfUnchanged = options.skipIfUnchanged;

  return buildMoonBundle(entrypoint, outputPath, buildOptions);
}

}  // namespace sun::cli
