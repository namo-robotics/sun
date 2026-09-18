// options.h — what the user asked for on the `sun` command line.
//
// Each command has a plain struct that the option parser fills in. The
// structs hold no behaviour and touch no global state, so they can be built
// by hand in tests and passed freely between the parser and the commands.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "driver/compiler.h"
#include "moon_bundling/moon_import.h"

namespace sun::cli {

/*
 * A command line that ends the run before any command starts: --help,
 * --version, or a mistake such as an unknown flag. It names the text to
 * print, the stream to print it on, and the process exit code.
 */
struct EarlyExit {
  enum class Stream { Out, Err };

  int exitCode = 0;
  Stream stream = Stream::Err;
  std::string text;
};

/*
 * Flags that both the default command and `sun test` accept.
 */
struct SharedOptions {
  std::vector<sun::MoonImport> moonImports;  // --moon <spec>
  std::vector<std::string> libPaths;         // --lib-path <dir>
  // --path-var NAME=<dir>, in the order given so a repeated name keeps its
  // last value
  std::vector<std::pair<std::string, std::string>> pathVariables;
  bool debugMode = false;  // --debug
  bool optimize = true;    // cleared by -O0
  bool emitIR = false;     // --emit-ir
  bool debugInfo = false;  // -g
};

/*
 * The default command: run a program with the JIT, compile it, or bundle it
 * into a .moon library.
 */
struct BuildRunOptions {
  SharedOptions shared;
  std::vector<std::string> inputFiles;
  std::string outputFile;                // -o <file>
  std::string targetTriple;              // --target <triple>
  sun::LinkOptions linkOptions;          // -l, -L, --sysroot, static or dynamic
  bool compileMode = false;              // -c, implied by --emit-obj
  bool emitObjOnly = false;              // --emit-obj
  bool staticRequested = false;          // --static
  bool dynamicRequested = false;         // --dynamic
  bool emitMoon = false;                 // --emit-moon
  bool dumpProtoSun = false;             // --dump-proto-sun
  bool noTest = false;                   // --no-test
  bool skipIfUnchanged = false;          // --skip-if-unchanged
  std::string githubToken;               // --gh-token <tok>
  std::vector<std::string> programArgs;  // everything after --
  // True when the single input is a sun-config.json standing in for the
  // entrypoints it declares
  bool configInput = false;
};

/*
 * `sun test`. Tests always carry debug info, so shared.debugInfo is accepted
 * and ignored.
 */
struct TestOptions {
  SharedOptions shared;
  std::string inputFile;
  // Passed to the test runner's main: --test-sequential, --test-filter and
  // its pattern, then everything after --
  std::vector<std::string> forwardedArgs;
};

/*
 * `sun fmt`.
 */
struct FmtOptions {
  bool checkMode = false;  // --check: report instead of rewriting
  std::vector<std::string> inputs;
};

}  // namespace sun::cli
