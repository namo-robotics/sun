// jit_command.h — `sun <script.sun>`, which runs a program straight away.

#pragma once

#include "cli/options.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/**
 * sun [options] <script.sun>... [-- args...]
 * Compiles the input files in memory and runs main with the JIT. The
 * arguments after -- reach main(argc, argv), and main's i32 result becomes
 * the exit code. A sun-config.json input runs its single binary entrypoint.
 */
int runJitCommand(const BuildRunOptions& options);

}  // namespace sun::cli
