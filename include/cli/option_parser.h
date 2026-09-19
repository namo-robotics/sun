// option_parser.h — turn `sun` command-line arguments into option structs.
//
// Parsing reads the arguments and nothing else: it prints nothing and
// changes no global state. When the command line ends the run early (--help,
// --version, an unknown flag, a rejected combination of flags) the parse
// function returns an EarlyExit describing what to print; otherwise it
// returns nothing and the options are ready to use. That makes every rule of
// the command line checkable from a unit test.
//
// Arguments are the ones after the command: for `sun -c app.sun` that is
// {"-c", "app.sun"}, for `sun test app.sun` it is {"app.sun"}.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cli/options.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/**
 * Parse the default command (run, compile or bundle) and check the result
 * with validateBuildRunOptions. programName appears in the help text.
 */
std::optional<EarlyExit> parseBuildRunArguments(
    const std::string& programName, const std::vector<std::string>& args,
    BuildRunOptions& options);

/**
 * Find the first combination of flags that makes no sense, such as --static
 * without -c. Returns nothing when the options are consistent.
 */
std::optional<EarlyExit> validateBuildRunOptions(
    const BuildRunOptions& options, const std::string& programName);

/**
 * Parse `sun test`.
 */
std::optional<EarlyExit> parseTestArguments(
    const std::vector<std::string>& args, TestOptions& options);

/**
 * Parse `sun fmt`.
 */
std::optional<EarlyExit> parseFmtArguments(const std::vector<std::string>& args,
                                           FmtOptions& options);

}  // namespace sun::cli
