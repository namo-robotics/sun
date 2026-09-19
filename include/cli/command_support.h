// command_support.h — small helpers that more than one `sun` command needs.
//
// The commands share a few chores: recognising a sun-config.json given in
// place of an entrypoint, pointing the library search at the folders the user
// named, and printing errors the same way everywhere.

#pragma once

#include <exception>
#include <string>

#include "cli/options.h"
#include "driver/sun_config.h"
#include "support/error.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/**
 * True when the input argument is a sun-config.json rather than a .sun
 * entrypoint. A config with an entrypoints list stands in for its
 * entrypoints on the command line.
 */
bool isConfigInput(const std::string& input);

/**
 * Parse the config named on the command line and insist it declares
 * entrypoints — without them there is nothing to stand in for.
 */
sun::driver::SunConfig loadConfigInput(const std::string& input,
                                       const std::string& targetTriple = "");

/**
 * Put the shared options into effect: define the path variables, then set up
 * the library search from the environment and the --lib-path folders.
 */
void applySharedSettings(const SharedOptions& shared);

/**
 * Put the default command's options into effect: the GitHub token, the
 * target the library search is for, and the shared settings.
 */
void applyBuildRunSettings(const BuildRunOptions& options);

/**
 * Print an early exit's text on its stream and return its exit code.
 */
int reportEarlyExit(const EarlyExit& earlyExit);

/**
 * Print a compile error and return the failing exit code.
 */
int reportSunError(const sun::support::SunError& error);

/**
 * Print any other exception, marked as an error, and return the failing exit
 * code.
 */
int reportUnexpectedError(const std::exception& error);

/**
 * True when the error says the program declares no test functions. Config
 * runs treat that as "nothing to do" rather than a failure, since a library
 * may simply have no tests yet.
 */
bool isNoTestsError(const sun::support::SunError& error);

}  // namespace sun::cli
