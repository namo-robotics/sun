#pragma once

#include <string>
#include <vector>

/** Coordinates compilation and generated source inputs. */
namespace sun::driver {

/** Inputs for generating one explicit Sun module from a C header. */
struct CBindingOptions {
  std::string header;
  std::string target;
  std::string module;
  std::string output;
  std::vector<std::string> allow;
  std::vector<std::string> clangArgs;
};

/** Parses a C header with libclang and atomically writes checked Sun bindings.
 */
void generateCBinding(const CBindingOptions& options);

/** Writes configured C bindings before resolving or reading manifest sources.
 * Unchanged outputs are preserved. Only manifest-listed files are compiled.
 * Header generation uses the compiler's linked libclang. */
void generateCBindings(const std::string& baseDir,
                       const std::string& targetTriple);

}  // namespace sun::driver
