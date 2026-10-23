#include "abi/support_manifest.h"

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

/** Validates compiler capabilities against concrete implementation bindings. */
namespace sun::abi {
void validateCompilerSupport(const proto::CompilerAbiSupport& support) {
  // This registry describes code that exists, not configurable compatibility
  // promises.
  const std::map<std::pair<std::string, std::string>, std::string> bindings{
      {{"moon.interface", "1"}, "moon_interface_v1"},
      {{"moon.metadata", "1"}, "moon_metadata_v1"},
      {{"llvm.bitcode", "experimental"}, "llvm_bitcode_experimental"},
      {{"sun.experimental.semantics", "experimental"},
       "sun_semantics_experimental"},
      {{"native.elf.x86_64.declared", "experimental"},
       "native_elf_declared_experimental"},
      {{"native.elf.aarch64.declared", "experimental"},
       "native_elf_declared_experimental"},
      {{"native.macho.x86_64.declared", "experimental"},
       "native_macho_declared_experimental"},
      {{"native.macho.aarch64.declared", "experimental"},
       "native_macho_declared_experimental"},
      {{"native.runtime.linker", "experimental"}, "native_linker_experimental"},
      {{"native.runtime.initializers", "experimental"},
       "native_initializers_experimental"},
      {{"native.runtime.thread_local", "experimental"},
       "native_thread_local_experimental"},
      {{"native.runtime.atexit", "experimental"},
       "native_atexit_experimental"}};
  if (support.schema() != 1 ||
      !support.GetReflection()->GetUnknownFields(support).empty()) {
    throw std::runtime_error("Unsupported compiler ABI support schema");
  }
  std::set<std::pair<std::string, std::string>> versions;
  std::set<std::string> names;
  for (const auto& rule : support.rules()) {
    const auto key = std::make_pair(rule.name(), rule.version());
    const auto binding = bindings.find(key);
    if (!rule.GetReflection()->GetUnknownFields(rule).empty() ||
        binding == bindings.end() || binding->second != rule.binding()) {
      throw std::runtime_error("Unavailable compiler ABI implementation: " +
                               rule.name());
    }
    if (!versions.insert(key).second) {
      throw std::runtime_error("Duplicate compiler ABI rule: " + rule.name());
    }
    names.insert(rule.name());
  }
  std::set<std::string> defaults;
  for (const auto& rule : support.emission_defaults()) {
    if (!rule.GetReflection()->GetUnknownFields(rule).empty() ||
        !versions.contains({rule.name(), rule.version()})) {
      throw std::runtime_error("Unsupported compiler ABI emission default: " +
                               rule.name());
    }
    if (!defaults.insert(rule.name()).second) {
      throw std::runtime_error("Duplicate compiler ABI emission default: " +
                               rule.name());
    }
  }
  if (names.empty() || names != defaults) {
    throw std::runtime_error(
        "Every compiler ABI rule needs an emission default");
  }
  // All current compilation paths are mandatory; omission cannot disable their
  // checks.
  for (const auto& [key, binding] : bindings) {
    if (!names.contains(key.first)) {
      throw std::runtime_error("Missing compiler ABI implementation: " +
                               key.first);
    }
  }
}
}  // namespace sun::abi
