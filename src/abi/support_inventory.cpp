#include "abi/support_inventory.h"

#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include "abi/compatibility.h"
#include "abi/producer_identity.h"

/** Publishes implemented compatibility rules independently of producer age. */
namespace sun::abi {
std::map<std::string, std::string> supportedRules() {
  const auto& implementation = compatibilityImplementation();
  return {{"moon.interface", "1"}, {"moon.metadata", "1"},
          {"llvm.bitcode", implementation},
          {"sun.experimental.semantics", implementation},
          {"native.elf.x86_64.baseline", implementation},
          {"native.runtime.atexit", implementation}};
}

std::string compatibilitySupportJson() {
  const auto producer = currentProducerIdentity();
  const auto target = compatibilityTarget("x86_64-linux-gnu");
  llvm::json::Object rules;
  for (const auto& [name, implementation] : supportedRules()) rules[name] = implementation;
  llvm::json::Object profile{{"architecture", target.architecture()},
      {"platform_abi", target.platform_abi()}, {"data_layout", target.data_layout()},
      {"pointer_bits", target.pointer_bits()}, {"little_endian", target.little_endian()}};
  llvm::json::Array profiles;
  profiles.push_back(std::move(profile));
  llvm::json::Object inventory{{"schema", 1}, {"experimental", true},
      {"producer", llvm::json::Object{{"build", producer.build()},
          {"release_order", producer.release_order()}, {"release_series", producer.release_series()}}},
      {"profiles", std::move(profiles)}, {"rules", std::move(rules)},
      {"native_constraints", "Relocatable ELF; audited baseline integer instructions; isolated symbols; only explicitly recorded atexit runtime references; no TLS or initialization arrays"},
      {"support_window", "No fixed support window during the experimental stage"},
      {"rebuild_guidance", "Rebuild unsupported bundles from their original inputs; producer age does not override rejection"}};
  return llvm::formatv("{0:2}\n", llvm::json::Value(std::move(inventory))).str();
}
}
