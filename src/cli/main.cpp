// main.cpp — entry point of the `sun` executable.
//
// Picks the command the user asked for and hands over to it. Each command
// lives in its own file under src/cli/; the rules of the command line itself
// are in option_parser.cpp.

#include <string>
#include <vector>

#include "cli/bundle_command.h"
#include "cli/command_support.h"
#include "cli/compile_command.h"
#include "cli/config_build_command.h"
#include "cli/fmt_command.h"
#include "cli/jit_command.h"
#include "cli/option_parser.h"
#include "cli/test_command.h"

using namespace sun::cli;

namespace sun::cli {

// The default command: bundle, compile or run, depending on the flags.
static int runDefaultCommand(const std::string& programName,
                             const std::vector<std::string>& args) {
  BuildRunOptions options;
  if (auto earlyExit = parseBuildRunArguments(programName, args, options)) {
    return reportEarlyExit(*earlyExit);
  }
  applyBuildRunSettings(options);

  if (options.emitMoon) return runBundleCommand(options);
  if (options.compileMode && options.configInput) {
    return runConfigBuildCommand(options);
  }
  if (options.compileMode) return runCompileCommand(options);
  return runJitCommand(options);
}

}  // namespace sun::cli

int main(int argc, char* argv[]) {
  const std::string programName = argc > 0 ? argv[0] : "sun";
  std::vector<std::string> args;
  if (argc > 1) args.assign(argv + 1, argv + argc);

  // A subcommand is only recognised as the first argument
  if (!args.empty() && (args[0] == "fmt" || args[0] == "test")) {
    std::vector<std::string> commandArgs(args.begin() + 1, args.end());
    return args[0] == "fmt" ? sun::cli::runFmtCommand(commandArgs)
                            : sun::cli::runTestCommand(commandArgs);
  }
  return sun::cli::runDefaultCommand(programName, args);
}
