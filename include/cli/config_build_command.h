// config_build_command.h — `sun -c sun-config.json`, building a project.
//
// A sun-config.json lists a project's entrypoints. Building it produces every
// product the list names, and is the one place where compiling and bundling
// meet: a binary entrypoint is compiled, a library entrypoint is bundled.

#pragma once

#include "cli/options.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/**
 * sun -c sun-config.json
 * Builds every declared entrypoint, stopping at the first failure. A binary
 * becomes an executable plus its test binary. A library becomes a .moon
 * bundle plus, when it has tests, its test binary.
 */
int runConfigBuildCommand(const BuildRunOptions& options);

}  // namespace sun::cli
