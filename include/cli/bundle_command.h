// bundle_command.h — `sun --emit-moon`, which builds a .moon library.
//
// Bundling turns an entrypoint and the files its manifest names into one
// precompiled .moon library that other programs import. It is separate from
// compiling executables: nothing here links a binary.

#pragma once

#include <filesystem>
#include <string>

#include "cli/options.h"
#include "moon_bundling/moon_builder.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/**
 * Build one .moon bundle from an entrypoint with a manifest, printing what
 * went into it. With buildOptions.skipIfUnchanged, a bundle already built
 * from the same inputs is left alone and reported as up to date. Returns the
 * exit code.
 */
int buildMoonBundle(const std::string& entrypoint,
                    const std::filesystem::path& outputPath,
                    const sun::moon_bundling::MoonBuildOptions& buildOptions);

/**
 * sun --emit-moon [-o &lt;file&gt;] <entrypoint.sun>
 * Bundles the first input file, to the -o path or a default next to it.
 */
int runBundleCommand(const BuildRunOptions& options);

}  // namespace sun::cli
