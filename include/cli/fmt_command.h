// fmt_command.h — `sun fmt`, the source formatter command.

#pragma once

#include <string>
#include <vector>

namespace sun::cli {

// sun fmt [--check] <file.sun|directory>...
// Formats files in place; directories are searched recursively for .sun
// files. With --check nothing is rewritten and files that would change are
// listed instead. Every file is processed before exiting.
// Exit codes: 0 = clean/formatted, 1 = --check found differences,
// 2 = parse or I/O error. `args` are the arguments after `fmt`.
int runFmtCommand(const std::vector<std::string>& args);

}  // namespace sun::cli
