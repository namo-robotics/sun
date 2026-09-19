// usage.h — the help and version text of the `sun` command line.
//
// The text is produced as strings rather than printed directly, so the option
// parser can hand it back as part of an EarlyExit and tests can read it.

#pragma once

#include <string>

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/**
 * The full help text. programName is how the user invoked the compiler.
 */
std::string renderUsage(const std::string& programName);

/**
 * The line `sun --version` prints: version number and git commit hash.
 */
std::string renderVersionLine();

// Usage line of `sun fmt`.
extern const char* const kFmtUsage;

// Usage line of `sun test` shown after an unknown option; lists every flag.
extern const char* const kTestUsageFull;

// Usage line of `sun test` shown when no entrypoint was named.
extern const char* const kTestUsageBrief;

}  // namespace sun::cli
