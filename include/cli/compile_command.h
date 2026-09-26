// compile_command.h — `sun -c`, which compiles a program ahead of time.
//
// Compiling turns Sun source into an executable, or with --emit-obj into an
// object file. A program that declares tests also gets a second executable,
// its test binary, unless --no-test was given. It is separate from bundling:
// nothing here builds a .moon library.
//
// Each artifact records the hash of the inputs it was built from (see
// driver/input_hash.h), and an artifact whose recorded hash matches what this
// run would build from is left alone. --force-rebuild skips that check and
// builds everything; the record is still written.

#pragma once

#include <string>
#include <vector>

#include "cli/options.h"
#include "driver/compiler.h"
#include "moon_bundling/moon_import.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/**
 * One compilation: which files, what to call the outputs, and the flags that
 * shape the build.
 */
struct CompileJob {
  std::vector<std::string> inputFiles;
  std::string outputFile;      // production artifact (resolved, non-empty)
  std::string testBinaryName;  // empty: outputFile + "_test"
  std::string targetTriple;
  sun::driver::LinkOptions baseLinkOpts;
  std::vector<sun::moon_bundling::MoonImport> moonImports;
  bool emitObjOnly = false;
  bool emitIR = false;
  bool debugMode = false;
  bool debugInfo = false;
  bool optimize = true;
  bool dumpProtoSun = false;
  bool noTest = false;
  // A selected distribution must have a production executable to include.
  bool requireProductionArtifact = false;
  // Build every artifact even when the one on disk is up to date
  bool forceRebuild = false;
};

/**
 * The default artifact name for an entrypoint: its path without the .sun
 * extension.
 */
std::string deriveOutputName(const std::string& entrypoint);

/**
 * Fill a job from the command-line options. The output names are copied as
 * given, so the caller still has to settle them.
 */
CompileJob makeCompileJob(const BuildRunOptions& options);

/**
 * Compile one entrypoint: the production executable (when there is a main,
 * or when there are no tests to build instead) plus the test binary (when
 * the program has tests and --no-test was not given). Prints errors and
 * returns the exit code.
 */
int compileEntrypoint(const CompileJob& job);

/**
 * Compile only the job's test binary: tests kept, the runner synthesized,
 * linked to the job's test binary name. `hasExecutable` says whether the
 * program also yields an executable, which the test binary records for later
 * runs. Throws SunError like any compile — including "no test functions
 * found" when the program has no tests; the caller decides what that means
 * for it.
 */
int compileTestBinary(const CompileJob& job, bool hasExecutable = true);

/**
 * sun -c [-o &lt;file&gt;] <script.sun>...  and  sun --emit-obj ...
 * Compiles the input files, naming the output after the first one unless -o
 * says otherwise.
 */
int runCompileCommand(const BuildRunOptions& options);

}  // namespace sun::cli
