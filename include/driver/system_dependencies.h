#pragma once

#include "driver/sun_config.h"

/** Checks and explicitly installs dependencies supplied by the host system. */
namespace sun::driver {

/** Runs declared checks and validates cached Debian artifacts against installed packages.
 * System dependencies without a check are left to the package manager.
 */
void checkSystemDependencies(const SunConfig& config);

/** Requests native dependency installation, or prints a plan when dryRun is true.
 * Passing checks skip installation; unchecked packages go to the package manager.
 * Refuses host package-manager operations for a foreign compilation target.
 */
void installSystemDependencies(const SunConfig& config, bool dryRun, bool yes);

/** Describes the projects available to dependency installation. */
struct ConfigGraph;

/** Installs requirements and prebuilt artifacts, fetching source configs without builds. */
void installConfigDependencies(const ConfigGraph& graph,
                               const std::vector<SunConfig>& configs,
                               bool dryRun, bool yes);
}  // namespace sun::driver
