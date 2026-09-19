// test_command.cpp — `sun test`, which runs a program's test functions.

#include "cli/test_command.h"

#include "cli/command_support.h"
#include "cli/option_parser.h"
#include "cli/program_arguments.h"
#include "driver/driver.h"
#include "llvm/Support/raw_ostream.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/** Keeps the implementation helpers in this file private to this translation unit. */
namespace {

/**
 * Compile one entrypoint with tests enabled and JIT-run the synthesized
 * runner. Returns its exit code: 0 iff every selected test passed. With
 * skipWhenNoTests (config runs, where a library may simply have no tests
 * yet) an entrypoint without tests reports itself and counts as passing;
 * without it that stays the usual error.
 */
int runTestEntrypoint(const std::string& inputFile, const TestOptions& options,
                      bool skipWhenNoTests = false) {
  // The runner reads its flags from main(argc, argv), argv[0] being the
  // entrypoint file, same as ordinary JIT execution.
  ProgramArguments programArgs(inputFile, options.forwardedArgs);

  try {
    auto driver = sun::driver::Driver::createForJIT(
        "main_module", /*debugInfo=*/true, options.shared.optimize);
    driver->setTestHandling(sun::driver::Driver::TestHandling::Compile);
    driver->setDumpIR(options.shared.emitIR);
    if (options.shared.debugMode) {
      driver->setDebugMode(true, inputFile);
    }
    driver->setMoonImports(options.shared.moonImports);
    auto result =
        driver->executeFile(inputFile, programArgs.argc(), programArgs.argv());
    // The runner returns 0 when every test passed, 1 otherwise
    if (auto* code = std::get_if<int32_t>(&result)) {
      return *code;
    }
  } catch (const sun::support::SunError& e) {
    if (skipWhenNoTests && isNoTestsError(e)) {
      llvm::outs() << "no tests\n";
      return 0;
    }
    return reportSunError(e);
  } catch (const std::exception& e) {
    return reportUnexpectedError(e);
  }
  return 0;
}

/**
 * Run the tests of every entrypoint a config declares, in turn. Returns 0
 * only when every suite passes.
 */
int runConfigTests(const TestOptions& options) {
  try {
    sun::driver::SunConfig config = loadConfigInput(options.inputFile);
    int failures = 0;
    for (const auto& entry : config.entrypoints) {
      if (config.entrypoints.size() > 1) {
        // Flushed so the header lands before the runner's own stdout.
        llvm::outs() << "== " << entry.path << " ==\n";
        llvm::outs().flush();
      }
      if (runTestEntrypoint(entry.path, options,
                            /*skipWhenNoTests=*/true) != 0) {
        failures++;
      }
    }
    return failures > 0 ? 1 : 0;
  } catch (const sun::support::SunError& e) {
    return reportSunError(e);
  }
}

}  // namespace

/** Runs the test command and returns its process exit status. */
int runTestCommand(const std::vector<std::string>& args) {
  TestOptions options;
  if (auto earlyExit = parseTestArguments(args, options)) {
    return reportEarlyExit(*earlyExit);
  }
  applySharedSettings(options.shared);

  if (isConfigInput(options.inputFile)) {
    return runConfigTests(options);
  }
  return runTestEntrypoint(options.inputFile, options);
}

}  // namespace sun::cli
