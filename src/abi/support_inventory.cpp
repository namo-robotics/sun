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
/** Resolves manifest selectors to the identities recorded in bundle contracts. */
namespace {
/** Hashes the stable reader contract independently of compiler implementation. */
const std::string& bitcodeContractHash() {
  static const std::string identity =
      hashBitcodeReaderContract(compilerAbiSupport().bitcode_reader_contract());
  return identity;
}

/** Recognizes the wire encoding shared by contract hashes and fingerprints. */
bool isHash(const std::string& identity) {
  return identity.size() == 64 &&
         identity.find_first_not_of("0123456789abcdef") == std::string::npos;
}

/** Uses the same identity for supported rules and emitted requirements. */
std::string implementationIdentity(const std::string& name,
                                   const std::string& version) {
  if (name == "llvm.bitcode" && version == "reader-contract") {
    return bitcodeContractHash();
  }
  return version == "experimental" ? compatibilityImplementation() : version;
}
}  // namespace

const proto::CompilerAbiSupport& compilerAbiSupport() {
  static const auto support = [] {
    proto::CompilerAbiSupport result;
    if (!result.ParseFromArray(sunCompilerAbiSupport,
                               sizeof(sunCompilerAbiSupport))) {
      sun::support::logAndThrowError("Invalid embedded compiler ABI support");
    }
    validateCompilerSupport(result);
    return result;
  }();
  return support;
}

std::map<std::string, std::string> supportedRules() {
  std::map<std::string, std::string> rules;
  for (const auto& rule : compilerAbiSupport().rules()) {
    rules.emplace(rule.name(),
                  implementationIdentity(rule.name(), rule.version()));
  }
  return rules;
}

bool isExperimentalFingerprint(const std::string& name,
                               const std::string& implementation) {
  if (!isHash(implementation)) {
    return false;
  }
  for (const auto& rule : compilerAbiSupport().rules()) {
    if (rule.name() == name && rule.version() == "experimental") {
      return true;
    }
  }
  return false;
}

bool compatibleRuleImplementations(const std::string& name,
                                   const std::string& required,
                                   const std::string& supported) {
  return required == supported ||
         (name == "llvm.bitcode" && isHash(required) && isHash(supported)) ||
         (isExperimentalFingerprint(name, required) &&
          isExperimentalFingerprint(name, supported));
}

std::string emissionImplementation(const std::string& name) {
  for (const auto& rule : compilerAbiSupport().emission_defaults()) {
    if (rule.name() == name) {
      return implementationIdentity(rule.name(), rule.version());
    }
  }
  sun::support::logAndThrowError("No compiler ABI emission default for " +
                                 name);
}

std::string compatibilitySupportJson() {
  const auto producer = currentProducerIdentity();
  llvm::json::Object rules;
  for (const auto& [name, implementation] : supportedRules()) {
    rules[name] = implementation;
  }
  llvm::json::Array bindings;
  for (const auto& rule : compilerAbiSupport().rules()) {
    bindings.push_back(llvm::json::Object{{"name", rule.name()},
                                          {"version", rule.version()},
                                          {"binding", rule.binding()}});
  }
  llvm::json::Object defaults;
  for (const auto& rule : compilerAbiSupport().emission_defaults()) {
    defaults[rule.name()] = rule.version();
  }
  llvm::json::Array profiles;
  for (const auto* triple :
       {"x86_64-linux-gnu", "x86_64-linux-musl", "aarch64-linux-gnu",
        "aarch64-linux-musl", "x86_64-apple-darwin", "aarch64-apple-darwin"}) {
    const auto target = compatibilityTarget(triple);
    profiles.push_back(
        llvm::json::Object{{"architecture", target.architecture()},
                           {"platform_abi", target.platform_abi()},
                           {"data_layout", target.data_layout()},
                           {"pointer_bits", target.pointer_bits()},
                           {"little_endian", target.little_endian()}});
  }
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
       "Linux GNU/musl ELF and macOS Mach-O on x86-64/AArch64; trusted native "
       "code "
       "with declared "
       "CPU/runtime requirements; isolated "
       "symbols; external references recorded in the contract and resolved by "
       "the linker; native TLS and "
       "initialization require native linking; relocation support delegated to "
       "the linker"},
      {"cpu_selection",
       "Native linking uses the generic destination CPU, matching object "
       "emission; JIT uses host capabilities. Compiler AVX2/FMA kernels are "
       "optional only behind independently checked CPU/OS probe guards. "
       "Custom destination CPU/feature overrides are not supported."},
      {"support_window",
       "No fixed support window during the experimental stage"},
      {"rebuild_guidance",
       "Rebuild unsupported bundles from their original inputs; producer age "
       "does not override rejection"}};
  return llvm::formatv("{0:2}\n", llvm::json::Value(std::move(inventory)))
      .str();
}
}  // namespace sun::abi
