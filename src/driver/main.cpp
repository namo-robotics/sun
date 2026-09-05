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
#include "driver/depfile.h"
#include "driver/driver.h"
#include "driver/manifest_processor.h"
#include "driver/sun_config.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_builder.h"
#include "moon_bundling/moon_cache.h"
#include "moon_bundling/moon_import.h"
#include "parsing/formatter.h"
#include "parsing/parser.h"
#include "sun_version.h"
#include "support/error.h"
#include "support/sun_path.h"

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
  llvm::errs() << "  --debug           Generate debug output (ast.dot, ir.ll, "
                  "test_runner.sun) in <input>_debug/\n";
  llvm::errs() << "  --no-test         Do not also compile the test binary "
                  "when the program has tests\n";
  llvm::errs() << "  --depfile <file>  Write a Make-format dependency file "
                  "naming every input each\n";
  llvm::errs() << "                    artifact was built from (for Ninja "
                  "or Make; use with -c or --emit-moon)\n";
  llvm::errs() << "  --emit-moon       Compile to .moon precompiled library\n";
  llvm::errs() << "                    Use manifest { source_files: [...] } "
                  "to specify files to include\n";
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
                  "e.g. source_files: [\"$NAME/util.sun\"]\n";
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
  llvm::errs() << "  test [--test-sequential] [--test-filter <pattern>] "
                  "<script.sun> [-- args...]\n";
  llvm::errs() << "                    JIT-run the program's test functions "
                  "(parallel by default);\n";
  llvm::errs() << "                    exit 0 when every test passes\n";
  llvm::errs() << "\nArguments after the script file (or after --) are passed "
                  "to main(argc, argv).\n";
  llvm::errs() << "\nsun-config.json files in the entrypoint's folder and "
                  "its parents define sun_path,\npath_variables and "
                  "entrypoints (nearest definitions win; \"root\": true "
                  "stops the\nsearch). They override --path-var, editor "
                  "settings and the environment. A config\nthat declares "
                  "entrypoints can stand in for them: `sun -c "
                  "sun-config.json` builds\nevery product, `sun test "
                  "sun-config.json` runs every suite, and plain `sun\n"
                  "sun-config.json` runs the single binary entrypoint.\n";
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
    // *.inja.sun files are templates, not Sun programs: their splices
    // cannot parse, so the formatter leaves them alone.
    if (path.extension() == ".sun" &&
        std::filesystem::path(path.stem()).extension() != ".inja") {
      out.push_back(path.string());
    }
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

// True when the input argument is a sun-config.json rather than a .sun
// entrypoint. A config with an entrypoints list stands in for its
// entrypoints on the command line.
static bool isConfigInput(const std::string& input) {
  return std::filesystem::path(input).filename() == sun::SunConfig::kFileName;
}

// Parse the config named on the command line and insist it declares
// entrypoints — without them there is nothing to stand in for.
static sun::SunConfig loadConfigInput(const std::string& input) {
  sun::SunConfig config =
      sun::SunConfig::loadFile(std::filesystem::absolute(input));
  if (config.entrypoints.empty()) {
    logAndThrowError(input +
                     " declares no entrypoints; add an 'entrypoints' list or "
                     "name a .sun file directly");
  }
  return config;
}

// The default artifact name for an entrypoint: its path without the .sun
// extension.
static std::string deriveOutputName(const std::string& entrypoint) {
  std::string output = entrypoint;
  size_t dotPos = output.rfind(".sun");
  if (dotPos != std::string::npos && dotPos == output.length() - 4) {
    output = output.substr(0, dotPos);
  }
  return output;
}

// Compile one entrypoint with tests enabled and JIT-run the synthesized
// runner. Returns its exit code: 0 iff every selected test passed. With
// skipWhenNoTests (config runs, where a library may simply have no tests
// yet) an entrypoint without tests reports itself and counts as passing;
// without it that stays the usual error.
static int runTestEntrypoint(const std::string& inputFile,
                             const std::vector<sun::MoonImport>& moonImports,
                             const std::vector<char*>& forwarded,
                             bool debugMode, bool debugInfo, bool emitIR,
                             bool skipWhenNoTests = false) {
  // The runner reads its flags from main(argc, argv), argv[0] being the
  // entrypoint file, same as ordinary JIT execution.
  std::vector<char*> programArgv;
  programArgv.push_back(const_cast<char*>(inputFile.c_str()));
  int programArgc = 1;
  for (char* forwardedArg : forwarded) {
    programArgv.push_back(forwardedArg);
    programArgc++;
  }
  programArgv.push_back(nullptr);

  try {
    auto driver = Driver::createForJIT("main_module", debugInfo);
    driver->setTestHandling(Driver::TestHandling::Compile);
    driver->setDumpIR(emitIR);
    if (debugMode) {
      driver->setDebugMode(true, inputFile);
    }
    driver->setMoonImports(moonImports);
    auto result =
        driver->executeFile(inputFile, programArgc, programArgv.data());
    // The runner returns 0 when every test passed, 1 otherwise
    if (auto* code = std::get_if<int32_t>(&result)) {
      return *code;
    }
  } catch (const SunError& e) {
    if (skipWhenNoTests &&
        std::string(e.what()).find("no test functions found") !=
            std::string::npos) {
      llvm::outs() << "no tests\n";
      return 0;
    }
    std::cerr << e.what() << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}

// `sun test <entrypoint>`: compile with tests enabled and JIT-run the
// synthesized runner. Arguments after the file (and anything after --) are
// forwarded to the runner's main; it reads --test-sequential and
// --test-filter <pattern>. A sun-config.json as the entrypoint runs every
// configured entrypoint's tests in turn.
static int runTest(int argc, char* argv[]) {
  std::string inputFile;
  std::vector<sun::MoonImport> moonImports;
  std::vector<std::string> libPaths;
  std::vector<char*> forwarded;
  bool debugMode = false;
  bool debugInfo = false;
  bool emitIR = false;

  int i = 0;
  for (; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--") {
      ++i;
      break;
    } else if (arg == "--test-sequential") {
      forwarded.push_back(argv[i]);
    } else if (arg == "--test-filter" && i + 1 < argc) {
      forwarded.push_back(argv[i]);
      forwarded.push_back(argv[++i]);
    } else if (arg == "--debug") {
      debugMode = true;
    } else if (arg == "-g") {
      debugInfo = true;
    } else if (arg == "--emit-ir") {
      emitIR = true;
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
    } else if (arg[0] == '-') {
      llvm::errs() << "Unknown option for 'sun test': " << arg << "\n";
      llvm::errs() << "Usage: sun test [--test-sequential] "
                      "[--test-filter <pattern>] [--debug] [-g] "
                      "[--emit-ir] [--moon <spec>] [--lib-path <dir>] "
                      "<script.sun> [-- args...]\n";
      return 1;
    } else if (inputFile.empty()) {
      inputFile = arg;
    } else {
      llvm::errs() << "'sun test' takes one entrypoint file\n";
      return 1;
    }
  }
  for (; i < argc; ++i) forwarded.push_back(argv[i]);

  if (inputFile.empty()) {
    llvm::errs() << "Usage: sun test [--test-sequential] "
                    "[--test-filter <pattern>] <script.sun> [-- args...]\n";
    return 1;
  }

  sun::LibraryCache::instance().initFromEnvironment();
  for (const auto& libPath : libPaths) {
    sun::LibraryCache::instance().addSearchPath(libPath);
    sun::SunPath::addSearchPath(libPath);
  }

  if (!isConfigInput(inputFile)) {
    return runTestEntrypoint(inputFile, moonImports, forwarded, debugMode,
                             debugInfo, emitIR);
  }

  // A config input: run every configured entrypoint's tests in turn. The
  // exit code is 0 only when every suite passes.
  try {
    sun::SunConfig config = loadConfigInput(inputFile);
    int failures = 0;
    for (const auto& entry : config.entrypoints) {
      if (config.entrypoints.size() > 1) {
        // Flushed so the header lands before the runner's own stdout.
        llvm::outs() << "== " << entry.path << " ==\n";
        llvm::outs().flush();
      }
      if (runTestEntrypoint(entry.path, moonImports, forwarded, debugMode,
                            debugInfo, emitIR,
                            /*skipWhenNoTests=*/true) != 0) {
        failures++;
      }
    }
    return failures > 0 ? 1 : 0;
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}

// One -c compilation: which files, what to call the outputs, and the flags
// that shape the build.
struct CompileJob {
  std::vector<std::string> inputFiles;
  std::string outputFile;      // production artifact (resolved, non-empty)
  std::string testBinaryName;  // empty: outputFile + "_test"
  std::string targetTriple;
  sun::LinkOptions baseLinkOpts;
  std::vector<sun::MoonImport> moonImports;
  bool emitObjOnly = false;
  bool emitIR = false;
  bool debugMode = false;
  bool debugInfo = false;
  bool dumpProtoSun = false;
  bool noTest = false;
  sun::Depfile* depfile = nullptr;  // records output -> inputs when given
};

// Compile the job's test binary: tests kept, the runner synthesized, linked
// to the job's test binary name. Throws SunError like any compile —
// including "no test functions found" when the program has no tests; the
// caller decides what that means for it.
static int compileTestBinary(const CompileJob& job) {
  const std::string& inputFile = job.inputFiles[0];
  const std::string testOutput = job.testBinaryName.empty()
                                     ? job.outputFile + "_test"
                                     : job.testBinaryName;

  auto testDriver =
      Driver::createForAOT("test_module", job.targetTriple, job.debugInfo);
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

  if (job.inputFiles.size() > 1) {
    testDriver->compileFiles(job.inputFiles, job.moonImports);
  } else {
    testDriver->compileFile(inputFile);
  }

  if (job.emitIR) {
    testDriver->printUserDefinedIR();
  }

  sun::LinkOptions testLinkOpts = job.baseLinkOpts;
  const auto& testBundled = testDriver->getNativeArchivePaths();
  testLinkOpts.archives.insert(testLinkOpts.archives.end(), testBundled.begin(),
                               testBundled.end());
  std::string errorMsg;
  if (!sun::compileToExecutable(testDriver->getModule(), testOutput, errorMsg,
                                /*keepObjectFile=*/false, testLinkOpts)) {
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

// The -c flow for one entrypoint: the production executable (when there is
// a main, or when there are no tests to build instead) plus the test binary
// (when the program has tests and --no-test was not given).
static int runCompile(const CompileJob& job) {
  const std::string& inputFile = job.inputFiles[0];
  llvm::outs() << "Compiling: " << inputFile << " -> " << job.outputFile
               << "\n";
  // The production link appends its bundles' archives below; the test
  // binary links from the untouched base copy plus its own bundles.
  sun::LinkOptions linkOpts = job.baseLinkOpts;

  try {
    auto driver =
        Driver::createForAOT("main_module", job.targetTriple, job.debugInfo);
    if (job.debugMode) {
      driver->setDebugMode(true, inputFile);
    }
    driver->setMoonImports(job.moonImports);
    driver->setDumpProtoSun(job.dumpProtoSun);

    if (job.inputFiles.size() > 1) {
      driver->compileFiles(job.inputFiles, job.moonImports);
    } else {
      driver->compileFile(inputFile);
    }

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

    // Emit executable
    std::string errorMsg;
    bool success = true;
    if (!emitProduction) {
      llvm::outs() << "No main() found; emitting only the test binary\n";
    } else if (job.emitObjOnly) {
      success =
          sun::emitObjectFile(driver->getModule(), job.outputFile, errorMsg);
    } else {
      // Imported bundles may carry their own static libraries; link those
      // too, so a program using such a bundle needs no -l flags.
      const auto& bundled = driver->getNativeArchivePaths();
      linkOpts.archives.insert(linkOpts.archives.end(), bundled.begin(),
                               bundled.end());
      success = sun::compileToExecutable(driver->getModule(), job.outputFile,
                                         errorMsg,
                                         /*keepObjectFile=*/false, linkOpts);
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
    std::cerr << e.what() << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}

// Build a .moon bundle from an entrypoint with a manifest. With a depfile,
// records the bundle's inputs: its sources, the bundles it imports, its
// proto schemas and the native archives it carries.
static int buildMoonArtifact(const std::string& entrypoint,
                             const std::filesystem::path& outputPath,
                             const std::string& targetTriple, bool debugInfo,
                             bool dumpProtoSun,
                             const std::vector<sun::MoonImport>& extraMoons,
                             sun::Depfile* depfile) {
  llvm::outs() << "Creating moon: " << outputPath.string() << "\n";
  try {
    sun::MoonBuildOptions options;
    options.targetTriple = targetTriple;
    options.debugInfo = debugInfo;
    options.dumpProtoSun = dumpProtoSun;
    options.extraMoons = extraMoons;
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
    llvm::errs() << "Error: " << e.what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    llvm::errs() << "Error: " << e.what() << "\n";
    return 1;
  }
}

// -c with a config input: build every declared entrypoint. A library
// becomes a .moon bundle plus (when it has tests) its test binary; a binary
// becomes an executable plus its test binary.
static int runConfigCompile(const sun::SunConfig& config,
                            const CompileJob& base) {
  for (const auto& entry : config.entrypoints) {
    CompileJob job = base;
    job.inputFiles = {entry.path};
    job.outputFile = entry.outputName.empty() ? deriveOutputName(entry.path)
                                              : entry.outputName;
    job.testBinaryName = entry.testBinaryName;

    // Config-named outputs may sit in folders that do not exist yet (e.g.
    // "build/stdlib"); create them so the artifacts have somewhere to land.
    std::error_code ec;
    for (const std::string& artifact : {job.outputFile, job.testBinaryName}) {
      std::filesystem::path parent =
          std::filesystem::path(artifact).parent_path();
      if (!artifact.empty() && !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
      }
    }

    if (entry.type == sun::ConfigEntrypoint::Type::Library) {
      std::filesystem::path moonPath(job.outputFile);
      if (moonPath.extension() != ".moon") {
        moonPath += ".moon";
      }
      if (buildMoonArtifact(entry.path, moonPath, job.targetTriple,
                            job.debugInfo, job.dumpProtoSun, job.moonImports,
                            job.depfile) != 0) {
        return 1;
      }
      if (job.noTest) {
        continue;
      }
      // The bundle is the production artifact; the only executable a
      // library yields is its test binary, and a library without tests
      // yields none.
      try {
        if (compileTestBinary(job) != 0) {
          return 1;
        }
      } catch (const SunError& e) {
        if (std::string(e.what()).find("no test functions found") ==
            std::string::npos) {
          std::cerr << e.what() << std::endl;
          return 1;
        }
      } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
      }
      continue;
    }

    if (runCompile(job) != 0) {
      return 1;
    }
  }
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc >= 2 && std::string(argv[1]) == "fmt") {
    return runFmt(argc - 2, argv + 2);
  }
  if (argc >= 2 && std::string(argv[1]) == "test") {
    return runTest(argc - 2, argv + 2);
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
  bool noTest = false;
  std::string depfilePath;
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
    } else if (arg == "--no-test") {
      noTest = true;
    } else if (arg == "--depfile" && i + 1 < argc) {
      depfilePath = argv[++i];
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
  if (noTest && (!compileMode || emitObjOnly)) {
    llvm::errs() << "Error: --no-test only applies when linking an "
                    "executable; use it with -c\n";
    return 1;
  }
  if (sawStatic && (!compileMode || emitObjOnly)) {
    llvm::errs() << "Error: --static only applies when linking; use it with "
                    "-c\n";
    return 1;
  }
  if (!depfilePath.empty() && !compileMode && !emitMoon) {
    llvm::errs() << "Error: --depfile describes built artifacts; use it "
                    "with -c or --emit-moon\n";
    return 1;
  }
  // The depfile is written once every artifact has been built, so the rules
  // it holds describe a build that actually succeeded.
  sun::Depfile depfile;
  auto finish = [&](int rc) {
    if (rc != 0 || depfilePath.empty()) return rc;
    try {
      depfile.write(depfilePath);
    } catch (const SunError& e) {
      std::cerr << e.what() << std::endl;
      return 1;
    }
    return 0;
  };
  // macOS has no fully static binaries: Apple ships no static libSystem or
  // startup objects, and its linker rejects -static for executables.
  bool darwinTarget = sun::effectiveLinkTriple(targetTriple).isOSDarwin();
  if (sawStatic && darwinTarget) {
    llvm::errs() << "Error: --static is not supported for macOS targets\n";
    return 1;
  }
  // Linking is static by default: one self-contained binary, the deployment
  // shape embedded targets want. --dynamic restores shared-library linking
  // (needed for .so-only vendor libraries). macOS is always dynamic.
  linkOpts.staticLink = !sawDynamic && !darwinTarget;

  // Initialize library cache
  sun::LibraryCache::instance().setTargetTriple(targetTriple);
  sun::LibraryCache::instance().initFromEnvironment();
  for (const auto& libPath : libPaths) {
    sun::LibraryCache::instance().addSearchPath(libPath);
    // Manifest `libraries:` entries resolve through SunPath, so --lib-path
    // has to reach it too, not just the bundle cache.
    sun::SunPath::addSearchPath(libPath);
  }

  // A sun-config.json named as the input stands in for its declared
  // entrypoints: -c builds every product, plain run executes the single
  // binary entrypoint. (`sun test <config>` is handled in runTest.)
  bool configInput = inputFiles.size() == 1 && isConfigInput(inputFiles[0]);
  if (configInput && (emitMoon || emitObjOnly)) {
    llvm::errs() << "Error: a sun-config.json input works with -c or plain "
                    "run; libraries in its entrypoints already build their "
                    ".moon bundles under -c\n";
    return 1;
  }
  if (configInput && !outputFile.empty()) {
    llvm::errs() << "Error: -o does not combine with a sun-config.json "
                    "input; the config's output_name fields name the "
                    "artifacts\n";
    return 1;
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
    return finish(buildMoonArtifact(entrypoint, outputPath, targetTriple,
                                    debugInfo, dumpProtoSun, moonImports,
                                    depfilePath.empty() ? nullptr : &depfile));
  }

  // Normal compilation/execution modes
  if (inputFiles.empty()) {
    llvm::errs() << "Error: No input file specified.\n";
    printUsage(argv[0]);
    return 1;
  }

  std::string inputFile = inputFiles[0];

  // In run mode a config input resolves to its one binary entrypoint; a
  // config with several is ambiguous about what to run.
  if (configInput && !compileMode) {
    try {
      sun::SunConfig config = loadConfigInput(inputFile);
      std::vector<const sun::ConfigEntrypoint*> binaries;
      for (const auto& entry : config.entrypoints) {
        if (entry.type == sun::ConfigEntrypoint::Type::Binary) {
          binaries.push_back(&entry);
        }
      }
      if (binaries.size() != 1) {
        llvm::errs() << "Error: " << inputFile << " declares "
                     << binaries.size()
                     << " binary entrypoints; name the .sun file to run\n";
        return 1;
      }
      inputFile = binaries[0]->path;
    } catch (const SunError& e) {
      std::cerr << e.what() << std::endl;
      return 1;
    }
  }

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
  if (compileMode && !configInput && outputFile.empty()) {
    outputFile = deriveOutputName(inputFile);
    if (emitObjOnly) {
      outputFile += ".o";
    }
  }

  if (compileMode) {
    linkOpts.targetTriple = targetTriple;
    CompileJob job;
    job.inputFiles = inputFiles;
    job.outputFile = outputFile;
    job.targetTriple = targetTriple;
    job.baseLinkOpts = linkOpts;
    job.moonImports = moonImports;
    job.emitObjOnly = emitObjOnly;
    job.emitIR = emitIR;
    job.debugMode = debugMode;
    job.debugInfo = debugInfo;
    job.dumpProtoSun = dumpProtoSun;
    job.noTest = noTest;
    job.depfile = depfilePath.empty() ? nullptr : &depfile;

    if (configInput) {
      try {
        // The config names the outputs, so every artifact depends on it too
        depfile.addSharedInput(
            std::filesystem::absolute(inputFiles[0]).string());
        return finish(runConfigCompile(loadConfigInput(inputFiles[0]), job));
      } catch (const SunError& e) {
        std::cerr << e.what() << std::endl;
        return 1;
      }
    }
    return finish(runCompile(job));
  }

  // JIT execution mode (default)
  // dlopen any -l shared libraries into this process first: the JIT resolves
  // extern symbols by searching the current process, so they have to be
  // loaded before the module referencing them is materialized. A -l name
  // that resolves to a static archive instead cannot be dlopen'd; it is
  // registered with the JIT's own linker below, once the JIT exists.
  // A library that fails to load is only a warning: libc/libm are already
  // resident (and glibc's libm.so is a linker script dlopen cannot read), so
  // their symbols resolve anyway. If one is genuinely missing, the JIT
  // reports the unresolved symbol with more precision than a guess here.
  auto nativeLibs = sun::loadNativeLibraries(linkOpts);
  for (const auto& lib : nativeLibs.failed) {
    llvm::errs() << "Warning: could not load library '" << lib
                 << "'; continuing in case its symbols are already present\n";
  }

  try {
    auto driver = Driver::createForJIT("main_module", debugInfo);
    for (const auto& archive : nativeLibs.archives) {
      driver->addJITStaticLibrary(archive);
    }
    driver->setDumpIR(emitIR);
    driver->setDumpProtoSun(dumpProtoSun);
    if (debugMode) {
      driver->setDebugMode(true, inputFile);
    }
    driver->setMoonImports(std::move(moonImports));

    sun::SunValue result;
    if (inputFiles.size() > 1) {
      result =
          driver->executeFiles(inputFiles, {}, programArgc, programArgv.data());
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
