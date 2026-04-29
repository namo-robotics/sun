// main.cpp
#include <glob.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "compiler.h"
#include "driver.h"
#include "error.h"
#include "library_cache.h"
#include "metadata_extractor.h"
#include "moon.h"

static void printUsage(const char* programName) {
  llvm::errs() << "Usage: " << programName
               << " [options] <script.sun> [-- args...]\n";
  llvm::errs() << "Options:\n";
  llvm::errs() << "  -c, --compile     Compile to executable (default: JIT "
                  "execute)\n";
  llvm::errs() << "  -o <file>         Output executable name (default: a.out "
                  "or based on input)\n";
  llvm::errs() << "  -S                Emit assembly file\n";
  llvm::errs() << "  --emit-obj        Emit object file only (do not link)\n";
  llvm::errs() << "  --emit-ir         Print LLVM IR to stdout\n";
  llvm::errs() << "  --emit-moon     Compile to .moon precompiled library\n";
  llvm::errs()
      << "  --bundle          Bundle multiple .sun files into one .moon\n";
  llvm::errs() << "  --lib-path <dir>  Add directory to library search path\n";
  llvm::errs() << "  -h, --help        Show this help message\n";
  llvm::errs() << "\nArguments after the script file (or after --) are passed "
                  "to main(argc, argv).\n";
  llvm::errs() << "\nExamples:\n";
  llvm::errs()
      << "  sun program.sun                              # JIT execute\n";
  llvm::errs()
      << "  sun --emit-moon -o lib.moon module.sun   # Create library\n";
  llvm::errs()
      << "  sun --bundle -o stdlib.moon stdlib/*.sun   # Bundle library\n";
  llvm::errs() << "  sun --lib-path build/ program.sun            # Use "
                  "precompiled libs\n";
}

/// Expand glob patterns in input files (for --bundle)
static std::vector<std::string> expandGlobs(
    const std::vector<std::string>& patterns) {
  std::vector<std::string> result;

  for (const auto& pattern : patterns) {
    glob_t globResult;
    int ret =
        glob(pattern.c_str(), GLOB_TILDE | GLOB_NOCHECK, nullptr, &globResult);

    if (ret == 0) {
      for (size_t i = 0; i < globResult.gl_pathc; ++i) {
        result.push_back(globResult.gl_pathv[i]);
      }
      globfree(&globResult);
    } else {
      // If glob fails, keep the original pattern
      result.push_back(pattern);
    }
  }

  return result;
}

int main(int argc, char* argv[]) {
  // Parse command-line arguments
  std::string outputFile;
  std::vector<std::string> inputFiles;
  std::vector<std::string> libPaths;
  bool compileMode = false;
  bool emitObjOnly = false;
  bool emitMoon = false;
  bool bundleMode = false;
  bool emitIR = false;
  int programArgStart = -1;  // Index where program arguments start

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--") {
      // Everything after -- is passed to the Sun program
      if (i + 1 < argc) {
        programArgStart = i + 1;
      }
      break;
    } else if (arg == "-c" || arg == "--compile") {
      compileMode = true;
    } else if (arg == "-o" && i + 1 < argc) {
      outputFile = argv[++i];
    } else if (arg == "--emit-obj") {
      compileMode = true;
      emitObjOnly = true;
    } else if (arg == "--emit-moon") {
      emitMoon = true;
    } else if (arg == "--emit-ir") {
      emitIR = true;
    } else if (arg == "--bundle") {
      bundleMode = true;
    } else if (arg == "--lib-path" && i + 1 < argc) {
      libPaths.push_back(argv[++i]);
    } else if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg[0] == '-') {
      llvm::errs() << "Unknown option: " << arg << "\n";
      printUsage(argv[0]);
      return 1;
    } else {
      // Input file or pattern
      inputFiles.push_back(arg);

      // For non-bundle/moon modes, stop after first input file
      if (!bundleMode && !emitMoon && inputFiles.size() == 1) {
        // Check if next args are program args (not options)
        if (i + 1 < argc && argv[i + 1][0] != '-') {
          programArgStart = i + 1;
          break;
        }
      }
    }
  }

  // Initialize library cache
  sun::LibraryCache::instance().initFromEnvironment();
  for (const auto& libPath : libPaths) {
    sun::LibraryCache::instance().addSearchPath(libPath);
  }

  // Handle --bundle mode
  if (bundleMode) {
    if (inputFiles.empty()) {
      llvm::errs() << "Error: --bundle requires input files\n";
      return 1;
    }
    if (outputFile.empty()) {
      llvm::errs() << "Error: --bundle requires -o <output.moon>\n";
      return 1;
    }

    // Expand glob patterns
    std::vector<std::string> expandedFiles = expandGlobs(inputFiles);
    if (expandedFiles.empty()) {
      llvm::errs() << "Error: No input files found\n";
      return 1;
    }

    llvm::outs() << "Bundling " << expandedFiles.size() << " files into "
                 << outputFile << "\n";

    sun::SunLibWriter bundleWriter;

    for (const auto& file : expandedFiles) {
      llvm::outs() << "  Processing: " << file << "\n";

      try {
        auto metadataOpt = sun::extractMetadataFromFile(file);
        if (!metadataOpt) {
          llvm::errs() << "Error: Failed to parse " << file
                       << " for metadata\n";
          return 1;
        }
        auto metadata = *metadataOpt;

        auto driver = Driver::createForAOT("bundle_module");
        driver->compileFile(file);

        bundleWriter.addModule(file, driver->getModule(), metadata);
      } catch (const SunError& e) {
        llvm::errs() << "Error compiling " << file << ": " << e.what() << "\n";
        return 1;
      } catch (const std::exception& e) {
        llvm::errs() << "Error compiling " << file << ": " << e.what() << "\n";
        return 1;
      }
    }

    if (!bundleWriter.write(outputFile)) {
      llvm::errs() << "Error writing bundle: " << bundleWriter.getError()
                   << "\n";
      return 1;
    }

    llvm::outs() << "Successfully created: " << outputFile << "\n";
    return 0;
  }

  // Handle --emit-moon mode
  if (emitMoon) {
    if (inputFiles.empty()) {
      llvm::errs() << "Error: --emit-moon requires an input file\n";
      return 1;
    }
    if (outputFile.empty()) {
      // Derive from input file
      outputFile = inputFiles[0];
      size_t dotPos = outputFile.rfind(".sun");
      if (dotPos != std::string::npos) {
        outputFile = outputFile.substr(0, dotPos);
      }
      outputFile += ".moon";
    }

    llvm::outs() << "Creating moon: " << inputFiles[0] << " -> " << outputFile
                 << "\n";

    try {
      auto metadataOpt = sun::extractMetadataFromFile(inputFiles[0]);
      if (!metadataOpt) {
        llvm::errs() << "Error: Failed to parse " << inputFiles[0]
                     << " for metadata\n";
        return 1;
      }
      auto metadata = *metadataOpt;

      auto driver = Driver::createForAOT("moon_module");
      driver->compileFile(inputFiles[0]);

      sun::SunLibWriter writer;
      writer.addModule(inputFiles[0], driver->getModule(), metadata);

      if (!writer.write(outputFile)) {
        llvm::errs() << "Error writing moon: " << writer.getError() << "\n";
        return 1;
      }

      llvm::outs() << "Successfully created: " << outputFile << "\n";
      return 0;
    } catch (const SunError& e) {
      llvm::errs() << "Error: " << e.what() << "\n";
      return 1;
    } catch (const std::exception& e) {
      llvm::errs() << "Error: " << e.what() << "\n";
      return 1;
    }
  }

  // Normal compilation/execution modes
  if (inputFiles.empty()) {
    llvm::errs() << "Error: No input file specified.\n";
    printUsage(argv[0]);
    return 1;
  }

  std::string inputFile = inputFiles[0];

  // Build program arguments: argv[0] = script name, followed by remaining args
  std::vector<char*> programArgv;
  programArgv.push_back(const_cast<char*>(inputFile.c_str()));
  int programArgc = 1;

  if (programArgStart > 0) {
    for (int i = programArgStart; i < argc; ++i) {
      programArgv.push_back(argv[i]);
      programArgc++;
    }
  }

  // Null-terminate for C compatibility
  programArgv.push_back(nullptr);

  // Determine output filename if not specified
  if (compileMode && outputFile.empty()) {
    // Derive output name from input (remove .sun extension if present)
    outputFile = inputFile;
    size_t dotPos = outputFile.rfind(".sun");
    if (dotPos != std::string::npos && dotPos == outputFile.length() - 4) {
      outputFile = outputFile.substr(0, dotPos);
    }
    if (emitObjOnly) {
      outputFile += ".o";
    }
  }

  if (compileMode) {
    // Compilation mode: generate executable without JIT
    llvm::outs() << "Compiling: " << inputFile << " -> " << outputFile << "\n";

    try {
      auto driver = Driver::createForAOT("main_module");
      driver->compileFile(inputFile);

      // Print IR if requested (only user-defined, not imports)
      if (emitIR) {
        driver->printUserDefinedIR();
      }

      // Emit executable
      std::string errorMsg;
      bool success;
      if (emitObjOnly) {
        success =
            sun::emitObjectFile(driver->getModule(), outputFile, errorMsg);
      } else {
        success =
            sun::compileToExecutable(driver->getModule(), outputFile, errorMsg);
      }

      if (!success) {
        llvm::errs() << "Compilation failed: " << errorMsg << "\n";
        return 1;
      }

      llvm::outs() << "Successfully compiled to: " << outputFile << "\n";
      return 0;
    } catch (const SunError& e) {
      std::cerr << e.what() << std::endl;
      return 1;
    } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << std::endl;
      return 1;
    }
  }

  // JIT execution mode (default)
  try {
    auto driver = Driver::createForJIT("main_module");
    driver->setDumpIR(emitIR);
    driver->executeFile(inputFile, programArgc, programArgv.data());
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
