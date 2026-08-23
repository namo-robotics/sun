// main.cpp
#include <glob.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ast/manifest_ast.h"
#include "driver/compiler.h"
#include "driver/driver.h"
#include "driver/manifest_processor.h"
#include "support/error.h"
#include "parsing/formatter.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/moon_builder.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_cache.h"
#include "moon_bundling/moon_import.h"
#include "parsing/parser.h"
#include "support/sun_path.h"
#include "sun_version.h"

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
  llvm::errs() << "  --target <triple> Cross-compile for <triple> (e.g. "
                  "aarch64-linux-gnu)\n";
  llvm::errs() << "                    Works with -c (needs a cross "
                  "toolchain), --emit-obj and --emit-moon\n";
  llvm::errs() << "  --sysroot <dir>   Target root filesystem for cross "
                  "linking (passed to the linker)\n";
  llvm::errs() << "  --static          Link a self-contained binary (the "
                  "default; musl preferred when installed)\n";
  llvm::errs() << "  --dynamic         Link against shared libraries instead "
                  "(needed for .so-only libs)\n";
  llvm::errs() << "  --emit-ir         Print LLVM IR to stdout\n";
  llvm::errs() << "  --dump-proto-sun  Print the Sun source synthesized from "
                  "manifest protos\n";
  llvm::errs() << "  -g                Emit DWARF debug info (for gdb/lldb)\n";
  llvm::errs() << "  --debug           Generate debug output (ast.dot, ir.ll) "
                  "in <input>_debug/\n";
  llvm::errs() << "  --emit-moon       Compile to .moon precompiled library\n";
  llvm::errs() << "                    Use manifest { suns: [...] } to specify "
                  "files to include\n";
  llvm::errs() << "  --lib-path <dir>  Add directory to .moon library search "
                  "path\n";
  llvm::errs() << "  -l<name>          Link against native library <name> "
                  "(e.g. -lm)\n";
  llvm::errs() << "                    Used for C FFI: linked when compiling, "
                  "loaded when JITing\n";
  llvm::errs() << "  -L<dir>           Add directory to the native library "
                  "search path\n";
  llvm::errs() << "  --moon <spec>     Load precompiled .moon library\n";
  llvm::errs() << "                    Format: path.moon or "
                  "path.moon:module=alias\n";
  llvm::errs() << "  --gh-token <tok>  GitHub token for manifest moon urls "
                  "on private repos\n";
  llvm::errs() << "                    (default: GH_TOKEN or GITHUB_TOKEN "
                  "environment variable)\n";
  llvm::errs() << "  --path-var NAME=<dir>\n";
  llvm::errs() << "                    Define $NAME for manifest entries, "
                  "e.g. suns: [\"$NAME/util.sun\"]\n";
  llvm::errs() << "                    (undefined names fall back to the "
                  "environment)\n";
  llvm::errs() << "  -h, --help        Show this help message\n";
  llvm::errs() << "  --version         Print version and git commit hash\n";
  llvm::errs() << "\nSubcommands:\n";
  llvm::errs() << "  fmt [--check] <file.sun|directory>...\n";
  llvm::errs() << "                    Format files in place; directories are "
                  "searched recursively\n";
  llvm::errs() << "                    (--check: exit 1 if formatting would "
                  "change a file)\n";
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

static const char* kFmtUsage =
    "Usage: sun fmt [--check] <file.sun|directory>...\n";

// Collect .sun files under a directory, skipping hidden directories
// (.git, .cache, ...). Sorted so output order is deterministic.
static bool collectSunFiles(const std::string& dir,
                            std::vector<std::string>& out) {
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(dir, ec), end;
  if (ec) {
    llvm::errs() << dir << ": cannot read directory: " << ec.message() << "\n";
    return false;
  }
  size_t firstNew = out.size();
  for (; it != end; it.increment(ec)) {
    if (ec) {
      llvm::errs() << dir << ": error while scanning: " << ec.message() << "\n";
      return false;
    }
    const std::filesystem::path& path = it->path();
    std::string name = path.filename().string();
    if (it->is_directory(ec)) {
      if (name.size() > 1 && name[0] == '.') it.disable_recursion_pending();
      continue;
    }
    if (path.extension() == ".sun") out.push_back(path.string());
  }
  std::sort(out.begin() + firstNew, out.end());
  return true;
}

// sun fmt [--check] <file.sun|directory>...
// Directories are searched recursively for .sun files.
// Exit codes: 0 = clean/formatted, 1 = --check found differences,
// 2 = parse or I/O error. All files are processed before exiting.
static int runFmt(int argc, char* argv[]) {
  bool checkMode = false;
  std::vector<std::string> inputs;
  for (int i = 0; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--check") {
      checkMode = true;
    } else if (arg == "-h" || arg == "--help") {
      llvm::errs() << kFmtUsage;
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      llvm::errs() << "Unknown fmt option: " << arg << "\n";
      return 2;
    } else {
      inputs.push_back(arg);
    }
  }
  if (inputs.empty()) {
    llvm::errs() << kFmtUsage;
    return 2;
  }

  bool hadError = false;
  bool hadDiff = false;

  // Expand directories; explicitly named non-.sun files are reported (a
  // directory walk filters them silently instead)
  std::vector<std::string> files;
  for (const auto& input : inputs) {
    std::error_code ec;
    if (std::filesystem::is_directory(input, ec)) {
      if (!collectSunFiles(input, files)) hadError = true;
    } else if (!std::filesystem::exists(input, ec)) {
      llvm::errs() << input << ": no such file or directory\n";
      hadError = true;
    } else if (std::filesystem::path(input).extension() != ".sun") {
      llvm::errs() << input << ": skipped (not a .sun file)\n";
    } else {
      files.push_back(input);
    }
  }

  for (const auto& file : files) {
    std::ifstream in(file);
    if (!in) {
      llvm::errs() << file << ": cannot open file\n";
      hadError = true;
      continue;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string source = buffer.str();
    in.close();

    std::string formatted;
    try {
      formatted = sun::formatSource(source, file);
    } catch (const SunError& e) {
      llvm::errs() << file << ": " << e.what() << "\n";
      hadError = true;
      continue;
    }

    if (formatted == source) continue;
    hadDiff = true;
    if (checkMode) {
      llvm::outs() << file << ": needs formatting\n";
      continue;
    }

    // Atomic in-place rewrite: temp file in the same directory, then rename
    std::string tmpPath = file + ".fmt-tmp";
    {
      std::ofstream out(tmpPath, std::ios::trunc);
      if (!out) {
        llvm::errs() << file << ": cannot write " << tmpPath << "\n";
        hadError = true;
        continue;
      }
      out << formatted;
    }
    std::error_code ec;
    std::filesystem::rename(tmpPath, file, ec);
    if (ec) {
      llvm::errs() << file << ": rename failed: " << ec.message() << "\n";
      std::filesystem::remove(tmpPath, ec);
      hadError = true;
    }
  }

  if (hadError) return 2;
  if (checkMode && hadDiff) return 1;
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc >= 2 && std::string(argv[1]) == "fmt") {
    return runFmt(argc - 2, argv + 2);
  }

  // Parse command-line arguments
  std::string outputFile;
  std::string targetTriple;
  std::vector<std::string> inputFiles;
  std::vector<std::string> libPaths;
  sun::LinkOptions linkOpts;
  std::vector<sun::MoonImport> moonImports;
  bool compileMode = false;
  bool emitObjOnly = false;
  bool sawStatic = false;
  bool sawDynamic = false;
  bool emitMoon = false;
  bool emitIR = false;
  bool debugMode = false;
  bool debugInfo = false;
  bool dumpProtoSun = false;
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
    } else if (arg == "--target" && i + 1 < argc) {
      targetTriple = argv[++i];
    } else if (arg == "--sysroot" && i + 1 < argc) {
      linkOpts.sysroot = argv[++i];
    } else if (arg == "--static") {
      sawStatic = true;
    } else if (arg == "--dynamic") {
      sawDynamic = true;
    } else if (arg == "--emit-moon") {
      emitMoon = true;
    } else if (arg == "--emit-ir") {
      emitIR = true;
    } else if (arg == "--dump-proto-sun") {
      dumpProtoSun = true;
    } else if (arg == "-g") {
      debugInfo = true;
    } else if (arg == "--debug") {
      debugMode = true;
    } else if (arg == "--lib-path" && i + 1 < argc) {
      libPaths.push_back(argv[++i]);
    } else if (arg == "-l" && i + 1 < argc) {
      linkOpts.libraries.push_back(argv[++i]);
    } else if (arg.rfind("-l", 0) == 0 && arg.size() > 2) {
      linkOpts.libraries.push_back(arg.substr(2));
    } else if (arg == "-L" && i + 1 < argc) {
      linkOpts.searchPaths.push_back(argv[++i]);
    } else if (arg.rfind("-L", 0) == 0 && arg.size() > 2) {
      linkOpts.searchPaths.push_back(arg.substr(2));
    } else if (arg == "--moon" && i + 1 < argc) {
      auto moonImport = sun::parseMoonImportSpec(argv[++i]);
      if (!moonImport) {
        llvm::errs() << "Invalid --moon format: " << argv[i] << "\n";
        llvm::errs() << "Expected: path.moon or path.moon:module=alias\n";
        return 1;
      }
      moonImports.push_back(std::move(*moonImport));
    } else if (arg == "--gh-token" && i + 1 < argc) {
      sun::MoonCache::setGithubToken(argv[++i]);
    } else if (arg == "--path-var" && i + 1 < argc) {
      std::string spec = argv[++i];
      auto eq = spec.find('=');
      if (eq == std::string::npos || eq == 0) {
        llvm::errs() << "Invalid --path-var format: " << spec << "\n";
        llvm::errs() << "Expected: NAME=<dir>\n";
        return 1;
      }
      sun::ManifestProcessor::setPathVariable(spec.substr(0, eq),
                                              spec.substr(eq + 1));
    } else if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg == "--version") {
      llvm::outs() << "sun " << SUN_VERSION << " (" << SUN_GIT_HASH << ")\n";
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

  // Cross-compilation produces object files and .moon artifacts; the JIT can
  // only run host code.
  if (!targetTriple.empty() && !emitObjOnly && !emitMoon && !compileMode) {
    llvm::errs() << "Error: --target requires --emit-obj, -c or --emit-moon "
                    "(JIT execution is host-only)\n";
    return 1;
  }
  if (sawStatic && sawDynamic) {
    llvm::errs() << "Error: --static and --dynamic are mutually exclusive\n";
    return 1;
  }
  if (sawStatic && (!compileMode || emitObjOnly)) {
    llvm::errs() << "Error: --static only applies when linking; use it with "
                    "-c\n";
    return 1;
  }
  // Linking is static by default: one self-contained binary, the deployment
  // shape embedded targets want. --dynamic restores shared-library linking
  // (needed for .so-only vendor libraries).
  linkOpts.staticLink = !sawDynamic;

  // Initialize library cache
  sun::LibraryCache::instance().setTargetTriple(targetTriple);
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
    const std::string& entrypoint = inputFiles[0];
    std::filesystem::path outputPath =
        outputFile.empty() ? sun::MoonBuilder::defaultOutputPath(entrypoint)
                           : std::filesystem::path(outputFile);

    llvm::outs() << "Creating moon: " << outputPath.string() << "\n";
    try {
      sun::MoonBuildOptions options;
      options.targetTriple = targetTriple;
      options.debugInfo = debugInfo;
      options.dumpProtoSun = dumpProtoSun;
      options.extraMoons = moonImports;
      auto report = sun::MoonBuilder::build(entrypoint, outputPath, options);
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
      auto driver =
          Driver::createForAOT("main_module", targetTriple, debugInfo);
      if (debugMode) {
        driver->setDebugMode(true, inputFile);
      }
      driver->setMoonImports(moonImports);
      driver->setDumpProtoSun(dumpProtoSun);

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
        linkOpts.targetTriple = targetTriple;
        success = sun::compileToExecutable(driver->getModule(), outputFile,
                                           errorMsg, /*keepObjectFile=*/false,
                                           linkOpts);
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
  // dlopen any -l libraries into this process first: the JIT resolves extern
  // symbols by searching the current process, so they have to be loaded
  // before the module referencing them is materialized.
  // A library that fails to load is only a warning: libc/libm are already
  // resident (and glibc's libm.so is a linker script dlopen cannot read), so
  // their symbols resolve anyway. If one is genuinely missing, the JIT
  // reports the unresolved symbol with more precision than a guess here.
  for (const auto& lib : sun::loadDynamicLibraries(linkOpts)) {
    llvm::errs() << "Warning: could not load library '" << lib
                 << "'; continuing in case its symbols are already present\n";
  }

  try {
    auto driver = Driver::createForJIT("main_module", debugInfo);
    driver->setDumpIR(emitIR);
    driver->setDumpProtoSun(dumpProtoSun);
    if (debugMode) {
      driver->setDebugMode(true, inputFile);
    }
    driver->setMoonImports(std::move(moonImports));

    sun::SunValue result;
    if (inputFiles.size() > 1) {
      result = driver->executeFiles(inputFiles, {}, programArgc,
                                    programArgv.data());
    } else {
      result = driver->executeFile(inputFile, programArgc, programArgv.data());
    }
    // main()'s i32 result is the process exit code (like C)
    if (auto* code = std::get_if<int32_t>(&result)) {
      return *code;
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
