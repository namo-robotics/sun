// jit_command.cpp — `sun <script.sun>`, which runs a program straight away.

#include "cli/jit_command.h"

#include <optional>

#include "cli/command_support.h"
#include "cli/program_arguments.h"
#include "driver/compiler.h"
#include "driver/driver.h"
#include "llvm/Support/raw_ostream.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/** Keeps the implementation helpers in this file private to this translation unit. */
namespace {

/**
 * A config input resolves to its one binary entrypoint; a config with
 * several is ambiguous about what to run. Reports the problem and returns
 * nothing when there is no single answer.
 */
std::optional<std::string> findConfigBinary(const std::string& configFile) {
  try {
    sun::driver::SunConfig config = loadConfigInput(configFile);
    std::vector<const sun::driver::ConfigEntrypoint*> binaries;
    for (const auto& entry : config.entrypoints) {
      if (entry.type == sun::driver::ConfigEntrypoint::Type::Binary) {
        binaries.push_back(&entry);
      }
    }
    if (binaries.size() != 1) {
      llvm::errs() << "Error: " << configFile << " declares " << binaries.size()
                   << " binary entrypoints; name the .sun file to run\n";
      return std::nullopt;
    }
    return binaries[0]->path;
  } catch (const sun::support::SunError& e) {
    reportSunError(e);
    return std::nullopt;
  }
}

}  // namespace

/** Runs the jit command and returns its process exit status. */
int runJitCommand(const BuildRunOptions& options) {
  std::string inputFile = options.inputFiles[0];
  if (options.configInput) {
    std::optional<std::string> binary = findConfigBinary(inputFile);
    if (!binary) return 1;
    inputFile = *binary;
  }

  // argv[0] = script name, followed by the arguments after --
  ProgramArguments programArgs(inputFile, options.programArgs);

  // dlopen any -l shared libraries into this process first: the JIT resolves
  // extern symbols by searching the current process, so they have to be
  // loaded before the module referencing them is materialized. A -l name
  // that resolves to a static archive instead cannot be dlopen'd; it is
  // registered with the JIT's own linker below, once the JIT exists.
  // A library that fails to load is only a warning: libc/libm are already
  // resident (and glibc's libm.so is a linker script dlopen cannot read), so
  // their symbols resolve anyway. If one is genuinely missing, the JIT
  // reports the unresolved symbol with more precision than a guess here.
  auto nativeLibs = sun::driver::loadNativeLibraries(options.linkOptions);
  for (const auto& lib : nativeLibs.failed) {
    llvm::errs() << "Warning: could not load library '" << lib
                 << "'; continuing in case its symbols are already present\n";
  }

  try {
    auto driver = sun::driver::Driver::createForJIT(
        "main_module", options.shared.debugInfo, options.shared.optimize);
    for (const auto& archive : nativeLibs.archives) {
      driver->addJITStaticLibrary(archive);
    }
    driver->setDumpIR(options.shared.emitIR);
    driver->setDumpProtoSun(options.dumpProtoSun);
    if (options.shared.debugMode) {
      driver->setDebugMode(true, inputFile);
    }
    driver->setMoonImports(options.shared.moonImports);

    sun::driver::SunValue result;
    if (options.inputFiles.size() > 1) {
      result = driver->executeFiles(options.inputFiles, {}, programArgs.argc(),
                                    programArgs.argv());
    } else {
      result = driver->executeFile(inputFile, programArgs.argc(),
                                   programArgs.argv());
    }
    // main()'s i32 result is the process exit code (like C)
    if (auto* code = std::get_if<int32_t>(&result)) {
      return *code;
    }
  } catch (const sun::support::SunError& e) {
    return reportSunError(e);
  } catch (const std::exception& e) {
    return reportUnexpectedError(e);
  }
  return 0;
}

}  // namespace sun::cli
