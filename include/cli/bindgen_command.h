#pragma once

#include <string>
#include <vector>

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {
/** Generates explicit Sun binding files from selected C header declarations. */
int runBindgenCommand(const std::vector<std::string>& arguments);
}  // namespace sun::cli
