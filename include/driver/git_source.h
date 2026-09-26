#pragma once

#include <string>

#include "driver/sun_config.h"

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {

/** Rejects invalid Git URLs, invalid revisions, and escaping source paths. */
void validateGitSource(const ConfigEntrypoint& entry);

/** Fetches a versioned checkout once and returns its entrypoint's absolute
 * path. Uses SUN_GIT_CACHE or ~/.sun/cache/git and Git's normal authentication.
 * Refresh fetches a new snapshot while preserving previous checkouts.
 * Reports fetch failures and paths escaping the checkout as compiler errors. */
std::string resolveGitEntrypoint(const ConfigEntrypoint& entry,
                                 bool refresh = false);

}  // namespace sun::driver
