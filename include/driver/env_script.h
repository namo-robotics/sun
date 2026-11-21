#pragma once

#include <string>

/** Generates shell environments for configured build products. */
namespace sun::driver {

/** Composed projects and their shared output root. */
struct ConfigGraph;

/** Write a sourceable PATH script for installed projects in the selected build.
 * For example, a viewer binary becomes available after sourcing env.sh.
 * @param graph The composed projects and their shared output root.
 * @param targetTriple The target whose installed binaries should be exposed.
 */
void writeEnvScript(const ConfigGraph& graph, const std::string& targetTriple);

}  // namespace sun::driver
