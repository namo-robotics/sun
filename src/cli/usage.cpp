// usage.cpp — the help and version text of the `sun` command line.

#include "cli/usage.h"

#include "generated/sun_version.h"

namespace sun::cli {

namespace {

// Everything in the help text below its first line, which names the program.
const char* const kUsageBody =
    "Options:\n"
    "  -c, --compile     Compile to executable (default: JIT execute)\n"
    "  -o <file>         Output executable name (default: a.out or based on "
    "input)\n"
    "  -S                Emit assembly file\n"
    "  --emit-obj        Emit object file only (do not link)\n"
    "  --target <triple> Cross-compile for <triple> (e.g. aarch64-linux-gnu)\n"
    "                    Works with -c (needs a cross toolchain), --emit-obj "
    "and --emit-moon\n"
    "  --sysroot <dir>   Target root filesystem for cross linking (passed to "
    "the linker)\n"
    "  --static          Link a self-contained binary (the default; musl "
    "preferred when installed)\n"
    "  --dynamic         Link against shared libraries instead (needed for "
    ".so-only libs)\n"
    "  --emit-ir         Print LLVM IR to stdout\n"
    "  --dump-proto-sun  Print the Sun source synthesized from manifest "
    "protos\n"
    "  -g                Emit DWARF debug info (keeps optimization level)\n"
    "  -O0               Disable IR and backend optimizations\n"
    "  --debug           Generate debug output (ast.dot, ir.ll, "
    "test_runner.sun) in <input>_debug/\n"
    "  --no-test         Do not also compile the test binary when the program "
    "has tests\n"
    "  --depfile <file>  Write a Make-format dependency file naming every "
    "input each\n"
    "                    artifact was built from (for Ninja or Make; use with "
    "-c or --emit-moon)\n"
    "  --emit-moon       Compile to .moon precompiled library\n"
    "                    Use manifest { source_files: [...] } to specify files "
    "to include\n"
    "  --lib-path <dir>  Add directory to .moon library search path\n"
    "  -l<name>          Link against native library <name> (e.g. -lm)\n"
    "                    Used for C FFI: linked when compiling, loaded when "
    "JITing\n"
    "  -L<dir>           Add directory to the native library search path\n"
    "  --moon <spec>     Load precompiled .moon library\n"
    "                    Format: path.moon or path.moon:module=alias\n"
    "  --gh-token <tok>  GitHub token for manifest moon urls on private repos\n"
    "                    (default: GH_TOKEN or GITHUB_TOKEN environment "
    "variable)\n"
    "  --path-var NAME=<dir>\n"
    "                    Define $NAME for manifest entries, e.g. source_files: "
    "[\"$NAME/util.sun\"]\n"
    "                    (undefined names fall back to the environment)\n"
    "  -h, --help        Show this help message\n"
    "  --version         Print version and git commit hash\n"
    "\n"
    "Subcommands:\n"
    "  fmt [--check] <file.sun|directory>...\n"
    "                    Format files in place; directories are searched "
    "recursively\n"
    "                    (--check: exit 1 if formatting would change a file)\n"
    "  test [--test-sequential] [--test-filter <pattern>] <script.sun> [-- "
    "args...]\n"
    "                    JIT-run the program's test functions (parallel by "
    "default);\n"
    "                    exit 0 when every test passes\n"
    "\n"
    "Arguments after the script file (or after --) are passed to main(argc, "
    "argv).\n"
    "\n"
    "sun-config.json files in the entrypoint's folder and its parents define "
    "sun_path,\n"
    "path_variables and entrypoints (nearest definitions win; \"root\": true "
    "stops the\n"
    "search). They override --path-var, editor settings and the environment. A "
    "config\n"
    "that declares entrypoints can stand in for them: `sun -c sun-config.json` "
    "builds\n"
    "every product, `sun test sun-config.json` runs every suite, and plain "
    "`sun\n"
    "sun-config.json` runs the single binary entrypoint.\n"
    "\n"
    "Examples:\n"
    "  sun program.sun                              # JIT execute\n"
    "  sun --emit-moon -o lib.moon module.sun       # Create library\n"
    "  sun --lib-path build/ program.sun            # Use precompiled libs\n";

}  // namespace

const char* const kFmtUsage =
    "Usage: sun fmt [--check] <file.sun|directory>...\n";

const char* const kTestUsageFull =
    "Usage: sun test [--test-sequential] "
    "[--test-filter <pattern>] [--debug] [-g] [-O0] "
    "[--emit-ir] [--moon <spec>] [--lib-path <dir>] "
    "<script.sun> [-- args...]\n";

const char* const kTestUsageBrief =
    "Usage: sun test [--test-sequential] "
    "[--test-filter <pattern>] <script.sun> [-- args...]\n";

std::string renderUsage(const std::string& programName) {
  return "Usage: " + programName + " [options] <script.sun> [-- args...]\n" +
         kUsageBody;
}

std::string renderVersionLine() {
  return std::string("sun ") + SUN_VERSION + " (" + SUN_GIT_HASH + ")\n";
}

}  // namespace sun::cli
