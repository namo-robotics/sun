#pragma once

#include <string>
#include <vector>

/** Describes native libraries packaged into Moon bundles. */
namespace sun::moon_bundling {

/** A resolved native archive and the capabilities its compiled code requires.
 */
struct NativeArchive {
  std::string path;
  std::vector<std::string> cpuFeatures;
  std::vector<std::string> runtimeServices;
};

}  // namespace sun::moon_bundling
