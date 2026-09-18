// bundle_command.cpp — `sun --emit-moon`, which builds a .moon library.

#include "cli/bundle_command.h"

#include "cli/command_support.h"
#include "llvm/Support/raw_ostream.h"
#include "support/error.h"

namespace sun::cli {

int buildMoonBundle(const std::string& entrypoint,
                    const std::filesystem::path& outputPath,
                    const sun::MoonBuildOptions& buildOptions,
                    sun::Depfile* depfile) {
  llvm::outs() << "Creating moon: " << outputPath.string() << "\n";
  try {
    auto report = sun::MoonBuilder::build(entrypoint, outputPath, buildOptions);
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
    if (depfile) {
      std::vector<std::string> inputs = report.sunFiles;
      for (const auto& m : report.moonImports) inputs.push_back(m.path);
      inputs.insert(inputs.end(), report.protoFiles.begin(),
                    report.protoFiles.end());
      inputs.insert(inputs.end(), report.archiveFiles.begin(),
                    report.archiveFiles.end());
      depfile->addOutput(outputPath.string(), inputs);
    }
    return 0;
  } catch (const SunError& e) {
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

int runBundleCommand(const BuildRunOptions& options) {
  const std::string& entrypoint = options.inputFiles[0];
  std::filesystem::path outputPath =
      options.outputFile.empty()
          ? sun::MoonBuilder::defaultOutputPath(entrypoint)
          : std::filesystem::path(options.outputFile);

  sun::MoonBuildOptions buildOptions;
  buildOptions.targetTriple = options.targetTriple;
  buildOptions.debugInfo = options.shared.debugInfo;
  buildOptions.optimize = options.shared.optimize;
  buildOptions.dumpProtoSun = options.dumpProtoSun;
  buildOptions.extraMoons = options.shared.moonImports;

  sun::Depfile depfile;
  int exitCode =
      buildMoonBundle(entrypoint, outputPath, buildOptions,
                      options.depfilePath.empty() ? nullptr : &depfile);
  return writeDepfileOnSuccess(exitCode, depfile, options.depfilePath);
}

}  // namespace sun::cli
