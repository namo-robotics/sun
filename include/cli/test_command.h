// test_command.h — `sun test`, which runs a program's test functions.

#pragma once

#include <string>
#include <vector>

namespace sun::cli {

// sun test [options] <script.sun> [-- args...]
// Compiles the entrypoint with tests enabled and JIT-runs the synthesized
// runner. --test-sequential, --test-filter <pattern> and everything after --
// are forwarded to the runner's main. A sun-config.json as the entrypoint
// runs every configured entrypoint's tests in turn. Returns 0 only when
// every test passed. `args` are the arguments after `test`.
int runTestCommand(const std::vector<std::string>& args);

}  // namespace sun::cli
