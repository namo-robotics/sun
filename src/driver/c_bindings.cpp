#include "driver/c_bindings.h"

#include <filesystem>
#include <set>
#include <vector>

#include "driver/manifest_processor.h"
#include "support/error.h"
#include "support/target_os.h"

/** Coordinates compilation and generated source inputs. */
namespace sun::driver {
void generateCBindings(const std::string& baseDir,
                       const std::string& targetTriple) {
  const auto config = ManifestProcessor::configFor(baseDir, targetTriple);
  if (!config) return;
  std::set<std::string> outputs;
  std::vector<CBindingOptions> pending;
  for (const auto& [module, binding] : config->cBindings) {
    if (!binding.enabled) continue;
    const auto header = binding.skipIfUnavailable
                            ? ManifestProcessor::tryExpandPathVariables(
                                  binding.header, &*config)
                            : ManifestProcessor::expandPathVariables(
                                  binding.header, &*config);
    if (!header) continue;
    CBindingOptions options;
    options.header = ManifestProcessor::resolvePath(
        *header, binding.baseDir, &*config, targetTriple);
    if (binding.skipIfUnavailable && !std::filesystem::exists(options.header))
      continue;
    const auto output =
        (std::filesystem::path(binding.baseDir) /
         ManifestProcessor::expandPathVariables(binding.output, &*config))
            .string();
    if (std::filesystem::path(output).extension() != ".sun")
      sun::support::logAndThrowError(
          "c_bindings output must name a .sun file: " + output);
    if (!outputs.insert(std::filesystem::weakly_canonical(output).string())
             .second)
      sun::support::logAndThrowError("C bindings share the same output: " +
                                     output);
    options.target = sun::support::resolvedTargetTriple(targetTriple).str();
    options.module = module;
    options.output = output;
    options.allow = binding.allow;
    options.clangArgs.push_back("-working-directory=" + binding.baseDir);
    for (const auto& arg : binding.clangArgs)
      options.clangArgs.push_back(
          ManifestProcessor::expandPathVariables(arg, &*config));
    pending.push_back(std::move(options));
  }
  // Validate every output before writing any generated files.
  for (const auto& options : pending) generateCBinding(options);
}
}  // namespace sun::driver
