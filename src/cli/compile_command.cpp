// compile_command.cpp — `sun -c`, which compiles a program ahead of time.

#include "cli/compile_command.h"

#include <cstdlib>
#include <filesystem>

#include "cli/command_support.h"
#include "driver/build_record.h"
#include "driver/driver.h"
#include "driver/input_hash.h"
#include "driver/manifest_processor.h"
#include "llvm/Support/raw_ostream.h"

namespace sun::cli {

namespace {

// Compile the job's input files into the driver's module.
void compileInputs(Driver& driver, const CompileJob& job) {
  if (job.inputFiles.size() > 1) {
    driver.compileFiles(job.inputFiles, job.moonImports);
  } else {
    driver.compileFile(job.inputFiles[0]);
  }
}

// The job's link options plus the static libraries the driver's imported
// bundles carry, so a program using such a bundle needs no -l flags.
sun::LinkOptions makeLinkOptions(const CompileJob& job, const Driver& driver) {
  sun::LinkOptions linkOpts = job.baseLinkOpts;
  const auto& bundled = driver.getNativeArchivePaths();
  linkOpts.archives.insert(linkOpts.archives.end(), bundled.begin(),
                           bundled.end());
  return linkOpts;
}

// The name of the job's test binary.
std::string getTestOutputName(const CompileJob& job) {
  return job.testBinaryName.empty() ? job.outputFile + "_test"
                                    : job.testBinaryName;
}

// Whether the job's builds may be skipped. Skipping has to be asked for, and
// flags that print or write something besides the artifact ask for the work
// to be done anyway.
bool maySkip(const CompileJob& job) {
  return job.skipIfUnchanged && !job.emitIR && !job.debugMode &&
         !job.dumpProtoSun;
}

// The hash of everything the job reads to build its executable or object
// file, or with `forTests` its test binary. Resolves the manifest the way
// the driver will, so the files hashed are the files compiled.
std::string computeJobInputHash(const CompileJob& job, bool forTests) {
  namespace fs = std::filesystem;
  sun::BuildInputs inputs;
  inputs.artifactKind = forTests          ? "tests"
                        : job.emitObjOnly ? "object"
                                          : "executable";
  inputs.targetTriple = job.targetTriple;
  // Tests always carry debug info
  inputs.debugInfo = forTests || job.debugInfo;
  inputs.optimize = job.optimize;

  inputs.moonImports = job.moonImports;
  for (auto& moon : inputs.moonImports) {
    moon.path = sun::ManifestProcessor::resolvePath(
        moon.path, fs::current_path().string(), nullptr, job.targetTriple);
  }

  std::vector<std::string> sourceFiles = job.inputFiles;
  std::vector<std::string> protoFiles;
  std::vector<std::string> archiveFiles;
  const std::string& entrypoint = job.inputFiles[0];
  // Several input files are compiled as given; one is an entrypoint whose
  // manifest names the rest.
  if (job.inputFiles.size() == 1) {
    if (auto manifest = sun::ManifestProcessor::fromEntrypointFile(
            entrypoint, job.targetTriple)) {
      sourceFiles.insert(sourceFiles.end(), manifest->sunFiles.begin(),
                         manifest->sunFiles.end());
      if (forTests) {
        sourceFiles.insert(sourceFiles.end(), manifest->testSunFiles.begin(),
                           manifest->testSunFiles.end());
      }
      inputs.moonImports.insert(inputs.moonImports.end(),
                                manifest->moonImports.begin(),
                                manifest->moonImports.end());
      protoFiles = std::move(manifest->protoFiles);
      archiveFiles = std::move(manifest->archiveFiles);
    }
  }
  sun::addSourceDigests(inputs, sourceFiles, protoFiles,
                        fs::absolute(entrypoint).parent_path().string());
  for (const auto& archive : archiveFiles) {
    inputs.archives.emplace_back(
        fs::path(archive).filename().string(),
        sun::computeFileDigest(archive, "native archive"));
  }

  // How the artifact is linked. Native libraries are named, not read: a
  // system library changing underneath is outside what sun can see.
  const sun::LinkOptions& link = job.baseLinkOpts;
  for (const auto& library : link.libraries) {
    inputs.settings.emplace_back("library", library);
  }
  for (const auto& path : link.searchPaths) {
    inputs.settings.emplace_back("library-path", path);
  }
  inputs.settings.emplace_back("sysroot", link.sysroot);
  inputs.settings.emplace_back("static", link.staticLink ? "1" : "0");
  const char* linkDriver = std::getenv("SUN_CC");
  inputs.settings.emplace_back("link-driver", linkDriver ? linkDriver : "");
  return sun::computeInputHash(inputs);
}

// True when the artifact at `path` records `inputHash`. `record` receives
// whatever the artifact recorded.
bool isUpToDate(const std::string& path, const std::string& inputHash,
                std::optional<sun::BuildRecord>& record) {
  record = sun::readBuildRecord(path);
  return record && record->inputHash == inputHash;
}

}  // namespace

std::string deriveOutputName(const std::string& entrypoint) {
  std::string output = entrypoint;
  size_t dotPos = output.rfind(".sun");
  if (dotPos != std::string::npos && dotPos == output.length() - 4) {
    output = output.substr(0, dotPos);
  }
  return output;
}

CompileJob makeCompileJob(const BuildRunOptions& options) {
  CompileJob job;
  job.inputFiles = options.inputFiles;
  job.outputFile = options.outputFile;
  job.targetTriple = options.targetTriple;
  job.baseLinkOpts = options.linkOptions;
  job.baseLinkOpts.targetTriple = options.targetTriple;
  job.moonImports = options.shared.moonImports;
  job.emitObjOnly = options.emitObjOnly;
  job.emitIR = options.shared.emitIR;
  job.debugMode = options.shared.debugMode;
  job.debugInfo = options.shared.debugInfo;
  job.optimize = options.shared.optimize;
  job.dumpProtoSun = options.dumpProtoSun;
  job.noTest = options.noTest;
  job.skipIfUnchanged = options.skipIfUnchanged;
  return job;
}

int compileTestBinary(const CompileJob& job, bool hasExecutable) {
  const std::string& inputFile = job.inputFiles[0];
  const std::string testOutput = getTestOutputName(job);

  // Empty unless skipping was asked for: nothing is hashed or recorded then
  const std::string inputHash =
      job.skipIfUnchanged ? computeJobInputHash(job, /*forTests=*/true) : "";
  std::optional<sun::BuildRecord> existing;
  if (maySkip(job) && isUpToDate(testOutput, inputHash, existing)) {
    llvm::outs() << "Up to date: " << testOutput << "\n";
    return 0;
  }

  // Tests inherit production optimization and always carry debug info.
  auto testDriver = Driver::createForAOT("test_module", job.targetTriple,
                                         /*debugInfo=*/true, job.optimize);
  if (job.debugMode) {
    // Separate folder (<input>_test_debug/) so the production build's
    // artifacts survive; this one also carries test_runner.sun.
    std::filesystem::path inputPath(inputFile);
    testDriver->setDebugMode(true, (inputPath.parent_path() /
                                    (inputPath.stem().string() + "_test.sun"))
                                       .string());
  }
  testDriver->setMoonImports(job.moonImports);
  testDriver->setTestHandling(Driver::TestHandling::Compile);

  compileInputs(*testDriver, job);

  if (job.emitIR) {
    testDriver->printUserDefinedIR();
  }

  if (job.skipIfUnchanged) {
    sun::embedBuildRecord(testDriver->getModule(),
                          {inputHash, /*hasTests=*/true, hasExecutable});
  }
  std::string errorMsg;
  if (!sun::compileToExecutable(testDriver->getModule(), testOutput, errorMsg,
                                /*keepObjectFile=*/false,
                                makeLinkOptions(job, *testDriver),
                                job.optimize)) {
    llvm::errs() << "Test compilation failed: " << errorMsg << "\n";
    return 1;
  }
  llvm::outs() << "Successfully compiled test binary to: " << testOutput
               << "\n";
  return 0;
}

int compileEntrypoint(const CompileJob& job) {
  const std::string& inputFile = job.inputFiles[0];

  try {
    const bool wantTests = !job.noTest && !job.emitObjOnly;
    // Empty unless skipping was asked for: nothing is hashed or recorded then
    const std::string inputHash =
        job.skipIfUnchanged ? computeJobInputHash(job, /*forTests=*/false) : "";

    // Which artifacts a program yields is known only after compiling it, so
    // each one records it for the other: the executable says whether there
    // are tests, the test binary whether there is an executable.
    if (maySkip(job)) {
      std::optional<sun::BuildRecord> built;
      if (isUpToDate(job.outputFile, inputHash, built)) {
        llvm::outs() << "Up to date: " << job.outputFile << "\n";
        if (!wantTests || !built->hasTests) return 0;
        return compileTestBinary(job);
      }
      // A program of only tests has no executable to find up to date; its
      // test binary is the whole build.
      if (wantTests &&
          isUpToDate(getTestOutputName(job),
                     computeJobInputHash(job, /*forTests=*/true), built) &&
          !built->hasExecutable) {
        llvm::outs() << "Up to date: " << getTestOutputName(job) << "\n";
        return 0;
      }
    }

    llvm::outs() << "Compiling: " << inputFile << " -> " << job.outputFile
                 << "\n";
    auto driver = Driver::createForAOT("main_module", job.targetTriple,
                                       job.debugInfo, job.optimize);
    if (job.debugMode) {
      driver->setDebugMode(true, inputFile);
    }
    driver->setMoonImports(job.moonImports);
    driver->setDumpProtoSun(job.dumpProtoSun);

    compileInputs(*driver, job);

    // Print IR if requested (only user-defined, not imports)
    if (job.emitIR) {
      driver->printUserDefinedIR();
    }

    bool buildTests =
        driver->programHasTests() && !job.noTest && !job.emitObjOnly;
    // A program of only tests has no main; that is fine, the test binary
    // is the deliverable. Without tests a missing main stays a link error.
    bool hasMain = driver->getModule().getFunction("main") != nullptr;
    bool emitProduction = hasMain || !buildTests;

    if (emitProduction && job.skipIfUnchanged) {
      sun::embedBuildRecord(driver->getModule(),
                            {inputHash, driver->programHasTests(),
                             /*hasExecutable=*/true});
    }
    std::string errorMsg;
    bool success = true;
    if (!emitProduction) {
      llvm::outs() << "No main() found; emitting only the test binary\n";
    } else if (job.emitObjOnly) {
      success = sun::emitObjectFile(driver->getModule(), job.outputFile,
                                    errorMsg, job.optimize);
    } else {
      success = sun::compileToExecutable(
          driver->getModule(), job.outputFile, errorMsg,
          /*keepObjectFile=*/false, makeLinkOptions(job, *driver),
          job.optimize);
    }

    if (!success) {
      llvm::errs() << "Compilation failed: " << errorMsg << "\n";
      return 1;
    }
    if (emitProduction) {
      llvm::outs() << "Successfully compiled to: " << job.outputFile << "\n";
    }

    // A program with tests also gets a test binary, so `sun -c` leaves
    // both artifacts behind. --no-test skips this second compile entirely.
    if (buildTests) {
      return compileTestBinary(job, emitProduction);
    }
    return 0;
  } catch (const SunError& e) {
    return reportSunError(e);
  } catch (const std::exception& e) {
    return reportUnexpectedError(e);
  }
}

int runCompileCommand(const BuildRunOptions& options) {
  CompileJob job = makeCompileJob(options);
  if (job.outputFile.empty()) {
    job.outputFile = deriveOutputName(job.inputFiles[0]);
    if (job.emitObjOnly) {
      job.outputFile += ".o";
    }
  }
  return compileEntrypoint(job);
}

}  // namespace sun::cli
