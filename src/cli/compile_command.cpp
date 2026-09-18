// compile_command.cpp — `sun -c`, which compiles a program ahead of time.

#include "cli/compile_command.h"

#include <filesystem>

#include "cli/command_support.h"
#include "driver/driver.h"
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

}  // namespace

std::string deriveOutputName(const std::string& entrypoint) {
  std::string output = entrypoint;
  size_t dotPos = output.rfind(".sun");
  if (dotPos != std::string::npos && dotPos == output.length() - 4) {
    output = output.substr(0, dotPos);
  }
  return output;
}

CompileJob makeCompileJob(const BuildRunOptions& options,
                          sun::Depfile* depfile) {
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
  job.depfile = depfile;
  return job;
}

int compileTestBinary(const CompileJob& job) {
  const std::string& inputFile = job.inputFiles[0];
  const std::string testOutput = job.testBinaryName.empty()
                                     ? job.outputFile + "_test"
                                     : job.testBinaryName;

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
  if (job.depfile) {
    job.depfile->addOutput(testOutput, testDriver->getInputFiles());
  }
  return 0;
}

int compileEntrypoint(const CompileJob& job) {
  const std::string& inputFile = job.inputFiles[0];
  llvm::outs() << "Compiling: " << inputFile << " -> " << job.outputFile
               << "\n";

  try {
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
      if (job.depfile) {
        job.depfile->addOutput(job.outputFile, driver->getInputFiles());
      }
    }

    // A program with tests also gets a test binary, so `sun -c` leaves
    // both artifacts behind. --no-test skips this second compile entirely.
    if (buildTests) {
      return compileTestBinary(job);
    }
    return 0;
  } catch (const SunError& e) {
    return reportSunError(e);
  } catch (const std::exception& e) {
    return reportUnexpectedError(e);
  }
}

int runCompileCommand(const BuildRunOptions& options) {
  sun::Depfile depfile;
  CompileJob job =
      makeCompileJob(options, options.depfilePath.empty() ? nullptr : &depfile);
  if (job.outputFile.empty()) {
    job.outputFile = deriveOutputName(job.inputFiles[0]);
    if (job.emitObjOnly) {
      job.outputFile += ".o";
    }
  }
  return writeDepfileOnSuccess(compileEntrypoint(job), depfile,
                               options.depfilePath);
}

}  // namespace sun::cli
