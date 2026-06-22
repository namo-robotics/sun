// main.cpp
#include <glob.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ast/manifest_ast.h"
#include "compiler.h"
#include "driver.h"
#include "error.h"
#include "library_cache.h"
#include "metadata_extractor.h"
#include "moon/moon.h"
#include "moon_import.h"
#include "parser.h"
#include "sun_path.h"

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
  llvm::errs() << "  --debug           Generate debug output (ast.dot, ir.ll) "
                  "in <input>_debug/\n";
  llvm::errs() << "  --emit-moon       Compile to .moon precompiled library\n";
  llvm::errs() << "                    Use manifest { suns: [...] } to specify "
                  "files to include\n";
  llvm::errs() << "  --lib-path <dir>  Add directory to library search path\n";
  llvm::errs() << "  --moon <spec>     Load precompiled .moon library\n";
  llvm::errs() << "                    Format: path.moon or "
                  "path.moon:module=alias\n";
  llvm::errs() << "  -h, --help        Show this help message\n";
  llvm::errs() << "\nArguments after the script file (or after --) are passed "
                  "to main(argc, argv).\n";
  llvm::errs() << "\nExamples:\n";
  llvm::errs()
      << "  sun program.sun                              # JIT execute\n";
  llvm::errs()
      << "  sun --emit-moon -o lib.moon module.sun       # Create library\n";
  llvm::errs() << "  sun --lib-path build/ program.sun            # Use "
                  "precompiled libs\n";
}

/// Resolve a manifest path - first relative to baseDir, then via SUN_PATH.
static std::string resolveManifestPath(const std::string& path,
                                       const std::string& baseDir) {
  std::filesystem::path p(path);
  if (p.is_absolute()) {
    return path;
  }
  // First try relative to base directory
  auto relative = std::filesystem::path(baseDir) / p;
  if (std::filesystem::exists(relative)) {
    return relative.lexically_normal().string();
  }
  // Then try SUN_PATH
  auto resolved = sun::SunPath::resolve(path);
  if (!resolved.empty()) {
    return resolved.string();
  }
  // Return as-is (will error later if not found)
  return path;
}

/// Extract manifest from a parsed program.
static const ManifestAST* findManifest(const BlockExprAST& program) {
  for (const auto& stmt : program.getBody()) {
    if (stmt && stmt->getType() == ASTNodeType::MANIFEST) {
      return static_cast<const ManifestAST*>(stmt.get());
    }
  }
  return nullptr;
}

/// Parse a file and extract manifest information.
/// Returns true if manifest found, populates sunFiles and moonImports.
/// Paths are resolved relative to the file's directory.
static bool extractManifestFromFile(const std::string& filename,
                                    std::vector<std::string>& sunFiles,
                                    std::vector<sun::MoonImport>& moonImports) {
  std::filesystem::path filePath = std::filesystem::absolute(filename);
  std::string baseDir = filePath.parent_path().string();

  std::ifstream file(filename);
  if (!file.is_open()) {
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  auto parser = Parser::createStringParser(source);
  parser.setFilePath(filename);
  auto ast = parser.parseProgram();

  const auto* manifest = findManifest(*ast);
  if (!manifest) {
    return false;
  }

  // Process sun dependencies
  for (const auto& sunDep : manifest->getSuns()) {
    std::string resolved = resolveManifestPath(sunDep.path, baseDir);
    sunFiles.push_back(resolved);
  }

  // Process moon dependencies
  for (const auto& moonDep : manifest->getMoons()) {
    std::string resolved = resolveManifestPath(moonDep.path, baseDir);
    if (moonDep.rename.has_value()) {
      moonImports.emplace_back(resolved, moonDep.rename.value(),
                               moonDep.rename.value());
    } else {
      moonImports.emplace_back(resolved);
    }
  }

  return true;
}

int main(int argc, char* argv[]) {
  // Parse command-line arguments
  std::string outputFile;
  std::vector<std::string> inputFiles;
  std::vector<std::string> libPaths;
  std::vector<sun::MoonImport> moonImports;
  bool compileMode = false;
  bool emitObjOnly = false;
  bool emitMoon = false;
  bool emitIR = false;
  bool debugMode = false;
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
    } else if (arg == "--debug") {
      debugMode = true;
    } else if (arg == "--lib-path" && i + 1 < argc) {
      libPaths.push_back(argv[++i]);
    } else if (arg == "--moon" && i + 1 < argc) {
      auto moonImport = sun::parseMoonImportSpec(argv[++i]);
      if (!moonImport) {
        llvm::errs() << "Invalid --moon format: " << argv[i] << "\n";
        llvm::errs() << "Expected: path.moon or path.moon:module=alias\n";
        return 1;
      }
      moonImports.push_back(std::move(*moonImport));
    } else if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg[0] == '-') {
      llvm::errs() << "Unknown option: " << arg << "\n";
      printUsage(argv[0]);
      return 1;
    } else {
      // Input file
      inputFiles.push_back(arg);
    }
  }

  // Initialize library cache
  sun::LibraryCache::instance().initFromEnvironment();
  for (const auto& libPath : libPaths) {
    sun::LibraryCache::instance().addSearchPath(libPath);
  }

  // Handle --emit-moon mode (create .moon library from entrypoint with
  // manifest)
  if (emitMoon) {
    if (inputFiles.empty()) {
      llvm::errs() << "Error: --emit-moon requires an entrypoint file\n";
      return 1;
    }

    std::string entrypoint = inputFiles[0];
    std::filesystem::path entrypointPath =
        std::filesystem::absolute(entrypoint);

    if (outputFile.empty()) {
      // Derive from input file
      outputFile = entrypoint;
      size_t dotPos = outputFile.rfind(".sun");
      if (dotPos != std::string::npos) {
        outputFile = outputFile.substr(0, dotPos);
      }
      outputFile += ".moon";
    }

    // Extract manifest from entrypoint
    std::vector<std::string> sunFiles;
    std::vector<sun::MoonImport> manifestMoons;

    if (!extractManifestFromFile(entrypoint, sunFiles, manifestMoons)) {
      // No manifest - single file mode
      sunFiles.push_back(entrypointPath.string());
    } else {
      // Add entrypoint to the list
      sunFiles.insert(sunFiles.begin(), entrypointPath.string());
      // Merge manifest moons with CLI moons
      for (auto& m : manifestMoons) {
        moonImports.push_back(std::move(m));
      }
    }

    llvm::outs() << "Creating moon: " << outputFile << "\n";
    for (const auto& f : sunFiles) {
      llvm::outs() << "  Including: " << f << "\n";
    }
    for (const auto& m : moonImports) {
      llvm::outs() << "  Moon import: " << m.path << "\n";
    }

    try {
      sun::SunLibWriter bundleWriter;

      // Extract metadata from each file
      std::vector<sun::moon::ModuleMetadata> allMetadata;
      for (const auto& file : sunFiles) {
        auto metadataOpt = sun::extractMetadataFromFile(file);
        if (!metadataOpt) {
          llvm::errs() << "Error: Failed to parse " << file
                       << " for metadata\n";
          return 1;
        }
        allMetadata.push_back(*metadataOpt);
      }

      // Compile all files together
      auto driver = Driver::createForAOT("moon_module");
      driver->compileFiles(sunFiles, moonImports);

      // Add each module's metadata + the shared compiled LLVM module
      for (auto& metadata : allMetadata) {
        bundleWriter.addModule(driver->getModule(), metadata);
      }

      if (!bundleWriter.write(outputFile)) {
        llvm::errs() << "Error writing moon: " << bundleWriter.getError()
                     << "\n";
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
      if (debugMode) {
        driver->setDebugMode(true, inputFile);
      }
      driver->setMoonImports(moonImports);

      if (inputFiles.size() > 1) {
        driver->compileFiles(inputFiles, moonImports);
      } else {
        driver->compileFile(inputFile);
      }

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
    if (debugMode) {
      driver->setDebugMode(true, inputFile);
    }
    driver->setMoonImports(std::move(moonImports));

    if (inputFiles.size() > 1) {
      driver->executeFiles(inputFiles, {}, programArgc, programArgv.data());
    } else {
      driver->executeFile(inputFile, programArgc, programArgv.data());
    }
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
