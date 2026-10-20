#include "abi/support_inventory.h"

#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>

#include "abi/compatibility.h"
#include "abi/producer_identity.h"
#include "abi/support_manifest.h"
#include "generated/sun_abi_support.h"
#include "support/error.h"

/** Publishes implemented compatibility rules independently of producer age. */
namespace sun::abi {
const proto::CompilerAbiSupport& compilerAbiSupport() {
  static const auto support = [] {
    proto::CompilerAbiSupport result;
    if (!result.ParseFromArray(sunCompilerAbiSupport,
                               sizeof(sunCompilerAbiSupport)))
      sun::support::logAndThrowError("Invalid embedded compiler ABI support");
    validateCompilerSupport(result);
    return result;
  }();
  return support;
}

std::map<std::string, std::string> supportedRules() {
  std::map<std::string, std::string> rules;
  for (const auto& rule : compilerAbiSupport().rules())
    rules.emplace(rule.name(), rule.version() == "experimental"
                                   ? compatibilityImplementation()
                                   : rule.version());
  return rules;
}

std::string emissionImplementation(const std::string& name) {
  for (const auto& rule : compilerAbiSupport().emission_defaults())
    if (rule.name() == name)
      return rule.version() == "experimental" ? compatibilityImplementation()
                                              : rule.version();
  sun::support::logAndThrowError("No compiler ABI emission default for " +
                                 name);
}

std::string compatibilitySupportJson() {
  const auto producer = currentProducerIdentity();
  const auto target = compatibilityTarget("x86_64-linux-gnu");
  llvm::json::Object rules;
  for (const auto& [name, implementation] : supportedRules())
    rules[name] = implementation;
  llvm::json::Array bindings;
  for (const auto& rule : compilerAbiSupport().rules())
    bindings.push_back(llvm::json::Object{{"name", rule.name()},
                                          {"version", rule.version()},
                                          {"binding", rule.binding()}});
  llvm::json::Object defaults;
  for (const auto& rule : compilerAbiSupport().emission_defaults())
    defaults[rule.name()] = rule.version();
  llvm::json::Object profile{{"architecture", target.architecture()},
                             {"platform_abi", target.platform_abi()},
                             {"data_layout", target.data_layout()},
                             {"pointer_bits", target.pointer_bits()},
                             {"little_endian", target.little_endian()}};
  llvm::json::Array profiles;
  profiles.push_back(std::move(profile));
  llvm::json::Object inventory{
      {"schema", 1},
      {"experimental", true},
      {"producer",
       llvm::json::Object{{"build", producer.build()},
                          {"release_order", producer.release_order()},
                          {"release_series", producer.release_series()}}},
      {"profiles", std::move(profiles)},
      {"rules", std::move(rules)},
      {"implementation_bindings", std::move(bindings)},
      {"emission_defaults", std::move(defaults)},
      {"native_constraints",
       "Linux x86-64 GNU relocatable ELF; trusted native code with declared "
       "CPU/runtime requirements; isolated "
       "symbols; only explicitly recorded atexit runtime references; no TLS or "
       "initialization arrays; relocation support delegated to the linker"},
      {"support_window",
       "No fixed support window during the experimental stage"},
      {"rebuild_guidance",
       "Rebuild unsupported bundles from their original inputs; producer age "
       "does not override rejection"}};
  return llvm::formatv("{0:2}\n", llvm::json::Value(std::move(inventory)))
      .str();
}
}  // namespace sun::abi
