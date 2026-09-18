// bundle_command.h — `sun --emit-moon`, which builds a .moon library.
//
// Bundling turns an entrypoint and the files its manifest names into one
// precompiled .moon library that other programs import. It is separate from
// compiling executables: nothing here links a binary.

#pragma once

#include <filesystem>
#include <string>

#include "cli/options.h"
#include "driver/depfile.h"
#include "moon_bundling/moon_builder.h"

namespace sun::cli {

// Build one .moon bundle from an entrypoint with a manifest, printing what
// went into it. With a depfile, records the bundle's inputs: its sources,
// the bundles it imports, its proto schemas and the native archives it
// carries. Returns the exit code.
int buildMoonBundle(const std::string& entrypoint,
                    const std::filesystem::path& outputPath,
                    const sun::MoonBuildOptions& buildOptions,
                    sun::Depfile* depfile);

// sun --emit-moon [-o <file>] <entrypoint.sun>
// Bundles the first input file, to the -o path or a default next to it.
int runBundleCommand(const BuildRunOptions& options);

}  // namespace sun::cli
