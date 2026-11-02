#pragma once

#include "cli/options.h"

/** Builds products declared by ordinary or composed Sun configs. */
namespace sun::cli {

/** Builds the selected config graph through the shared product scheduler. */
int runConfigBuildCommand(const BuildRunOptions& options);

/** Supplies compiler settings to local dependency builds. */
void configureProductCompiler(const BuildRunOptions& options);

/** Resolves sun build arguments and invokes the ordinary config build path. */
int runBuildCommand(const std::vector<std::string>& args);

/** Resolves a dependency through the selected graph and prints its root as
 * JSON. */
int runResolveCommand(const std::vector<std::string>& args);

/** Prints resolved project paths without compiling or downloading dependencies.
 */
int runDescribeCommand(const std::vector<std::string>& args);

/** Prints a pre-resolved dependency root from the inherited script context. */
int runDependencyPathCommand(const std::vector<std::string>& args);

/** Compiles the current script target with inherited settings and extra flags.
 */
int runBuildCompileCommand(const std::vector<std::string>& args);

}  // namespace sun::cli
