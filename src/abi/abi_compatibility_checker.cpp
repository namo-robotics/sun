#include "abi/abi_compatibility_checker.h"

#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include "abi/boundary_requirements.h"
#include "abi/native_requirements.h"
#include "abi/producer_identity.h"
#include "abi/semantic_requirements.h"
#include "abi/support_inventory.h"
#include "moon_bundling/moon.h"
#include "support/error.h"
#include "support/terminal.h"

/** Owns compatibility policy independently of bundle container decoding. */
namespace sun::abi {

AbiCompatibilityChecker::AbiCompatibilityChecker(std::string targetTriple,
                                                 NativeExecution execution)
    : targetTriple_(std::move(targetTriple)), execution_(execution) {}

AbiCompatibilityChecker& AbiCompatibilityChecker::current() {
  thread_local AbiCompatibilityChecker checker;
  return checker;
}

void AbiCompatibilityChecker::beginCompilation(const std::string& targetTriple,
                                               NativeExecution execution) {
  targetTriple_ = targetTriple;
  execution_ = execution;
  byteHashes_.clear();
  warnedBundles_.clear();
  importedContracts_.clear();
  validatedBundles_.clear();
}

std::string AbiCompatibilityChecker::bytesHash(
    const moon_bundling::MoonReader& reader) const {
  const std::weak_ptr<const std::string> snapshot(reader.getBytes());
  if (auto found = byteHashes_.find(snapshot); found != byteHashes_.end())
    return found->second;
  return byteHashes_
      .emplace(snapshot, moon_bundling::computeSha256Hex(*reader.getBytes()))
      .first->second;
}

ValidatedBundle AbiCompatibilityChecker::validate(
    moon_bundling::MoonReader& reader) {
  return validate(reader, targetTriple_);
}

ValidatedBundle AbiCompatibilityChecker::validate(
    moon_bundling::MoonReader& reader, const std::string& targetTriple) {
  const auto digest = bytesHash(reader);
  const auto key =
      std::make_pair(digest, targetTriple + ":" +
                                 std::to_string(static_cast<int>(execution_)) +
                                 ":" + compatibilityImplementation());
  auto found = validatedBundles_.find(key);
  if (found == validatedBundles_.end()) {
    const auto modules = reader.listModules();
    const auto* metadata = &reader.getLibrary();
    if (!metadata || !metadata->has_bundle_contract())
      sun::support::logAndThrowError(
          "Dependency '" + reader.getPath().string() +
          "': compatibility cannot be established; missing bundle contract; "
          "rebuild the bundle");
    const auto& contract = metadata->bundle_contract();
    validateContract(contract, reader.getPath().string(), targetTriple);
    const std::set<std::string> semantics(contract.semantic_records().begin(),
                                          contract.semantic_records().end());
    bool metadataRule = false;
    for (const auto& rule : contract.requirements())
      if (rule.name() == "moon.metadata") metadataRule = true;
    if (!metadataRule)
      sun::support::logAndThrowError(
          "Dependency '" + reader.getPath().string() + "', contract " +
          contract.compatibility_hash() +
          ": missing moon.metadata requirement");
    std::vector<const moon::ModuleMetadata*> declarationModules;
    for (const auto& moduleKey : modules)
      declarationModules.push_back(reader.getMetadata(moduleKey));
    try {
      validateDeclarationGraph(declarationModules);
    } catch (const sun::support::SunError& error) {
      sun::support::logAndThrowError(
          "Dependency '" + reader.getPath().string() + "', contract " +
          contract.compatibility_hash() + ": " + error.what());
    }
    std::set<std::string> checked;
    for (const auto& moduleKey : modules) {
      try {
        if (!semantics.count(describeSemantics(*reader.getMetadata(moduleKey))))
          sun::support::logAndThrowError(
              "declarations or retained bodies differ from contract");
      } catch (const sun::support::SunError& error) {
        sun::support::logAndThrowError(
            "Dependency '" + reader.getPath().string() + "', contract " +
            contract.compatibility_hash() + ": " + error.what());
      }
      if (!checked.insert(reader.getBitcodeId(moduleKey)).second) continue;
      llvm::LLVMContext context;
      auto module = reader.loadModule(moduleKey, context);
      if (!module)
        sun::support::logAndThrowError(
            "Unsupported Moon payload in '" + reader.getPath().string() +
            "', contract " + contract.compatibility_hash() + ": " +
            reader.getError());
      if (auto error = module->materializeAll())
        sun::support::logAndThrowError(
            "Unsupported Moon payload in '" + reader.getPath().string() +
            "', contract " + contract.compatibility_hash() + ": " +
            llvm::toString(std::move(error)));
      validatePayload(contract, *module, reader.getPath().string(),
                      targetTriple);
    }
    if (!reader.getNativeArchives().empty()) {
      bool nativeRule = false;
      for (const auto& rule : contract.requirements())
        if (rule.name() == nativeArchiveRule(contract.target()))
          nativeRule = true;
      try {
        if (!nativeRule)
          sun::support::logAndThrowError("missing native archive requirements");
        std::vector<std::vector<char>> buffers(
            reader.getNativeArchives().size());
        std::vector<NativeArchiveInput> archives;
        size_t index = 0;
        for (const auto& entry : reader.getNativeArchives()) {
          auto& bytes = buffers[index++];
          if (!reader.readNativeArchive(entry, bytes))
            sun::support::logAndThrowError(reader.getError());
          archives.push_back(
              {llvm::MemoryBufferRef(
                   llvm::StringRef(bytes.data(), bytes.size()), entry.name),
               "$" + entry.archiveSetHash + "$_"});
        }
        std::set<std::string> runtimeServices;
        validateNativeArchives(archives, contract.target(), &runtimeServices);
        for (const auto& service : runtimeServices) {
          bool recorded = false;
          for (const auto& rule : contract.requirements())
            if (rule.name() == "native.runtime." + service) recorded = true;
          if (!recorded)
            sun::support::logAndThrowError(
                "missing native runtime requirement: " + service);
        }
      } catch (const sun::support::SunError& error) {
        sun::support::logAndThrowError(
            "Dependency '" + reader.getPath().string() + "', contract " +
            contract.compatibility_hash() + ": " + error.what());
      }
    }
    found = validatedBundles_
                .emplace(key, std::make_shared<const BundleContract>(contract))
                .first;
  }
  recordValidatedImport(*found->second, digest, reader.getPath().string());
  return ValidatedBundle(found->second);
}

void AbiCompatibilityChecker::validateContract(
    const BundleContract& contract, const std::string& bundle,
    const std::string& targetTriple) const {
  /** Reports a concrete unsupported requirement before semantic import. */
  auto fail = [&](const std::string& reason) {
    sun::support::logAndThrowError("Dependency '" + bundle +
                                   "' is incompatible; bundle contract " +
                                   contract.compatibility_hash() + ": " +
                                   reason + "; compilation stopped");
  };
  if (contract.encoding_schema() != 1 || !contract.experimental())
    fail("unsupported contract encoding or stability claim");
  if (hashBundleContract(contract) != contract.compatibility_hash())
    fail("contract description does not match its hash");
  auto expected = compatibilityTarget(targetTriple);
  auto actual = contract.target();
  actual.clear_cpu_features();
  if (expected.architecture() != actual.architecture() ||
      expected.platform_abi() != actual.platform_abi() ||
      expected.data_layout() != actual.data_layout() ||
      expected.pointer_bits() != actual.pointer_bits() ||
      expected.little_endian() != actual.little_endian())
    fail("incompatible target profile for current target '" + targetTriple +
         "'");
  if (!contract.target().cpu_features().empty() &&
      expected.architecture() !=
          llvm::Triple(llvm::sys::getDefaultTargetTriple()).getArchName().str())
    fail("CPU feature support cannot be established for a cross target");
  llvm::StringMap<bool> hostFeatures = llvm::sys::getHostCPUFeatures();
  for (const auto& feature : contract.target().cpu_features()) {
    if (!hostFeatures.lookup(feature))
      fail("unsupported CPU feature '" + feature + "'");
    expected.add_cpu_features(feature);
  }
  BundleContract derived;
  derived.set_encoding_schema(1);
  derived.set_experimental(true);
  *derived.mutable_target() = expected;
  const auto supportedImplementations = supportedRules();
  std::set<std::string> seen;
  for (const auto& rule : contract.requirements()) {
    if (!seen.insert(rule.name()).second)
      fail("duplicate required rule '" + rule.name() + "'");
    const auto found = supportedImplementations.find(rule.name());
    if (found == supportedImplementations.end())
      fail("unknown required capability '" + rule.name() + "'");
    const auto& implementation = found->second;
    if (rule.implementation() != implementation)
      fail("unsupported implementation of '" + rule.name() + "'");
    auto* supported = derived.add_requirements();
    supported->set_name(rule.name());
    supported->set_implementation(implementation);
  }
  for (const auto* mandatory :
       {"moon.interface", "llvm.bitcode", "sun.experimental.semantics"})
    if (!seen.count(mandatory))
      fail(std::string("missing required rule '") + mandatory + "'");
  if (execution_ == NativeExecution::JIT &&
      (seen.count("native.runtime.initializers") ||
       seen.count("native.runtime.thread_local")))
    fail(
        "native initialization or thread-local storage requires native "
        "linking; the JIT does not implement it");
  for (const auto& rule : contract.requirements())
    if (rule.name().starts_with("native.elf.") &&
        rule.name() != nativeArchiveRule(contract.target()))
      fail("native object rule does not match the target");
  // Boundary records are checked against decoded payloads by this checker. The
  // exact semantics closure supplies interpretation of retained generic ASTs.
  BoundaryRequirements boundaries;
  try {
    for (const auto& record : contract.boundary_records())
      boundaries.add(record);
  } catch (const sun::support::SunError& error) {
    fail(error.what());
  }
  for (const auto& record : boundaries.records())
    derived.add_boundary_records(record);
  for (const auto& record : contract.semantic_records())
    derived.add_semantic_records(record);
  if (hashBundleContract(derived) != contract.compatibility_hash())
    fail("derived contract hash differs");
}
void AbiCompatibilityChecker::validatePayload(
    const BundleContract& contract, const llvm::Module& module,
    const std::string& bundle, const std::string& targetTriple) const {
  /** Reports payload failures using the supplied bundle contract identity. */
  auto fail = [&](const std::string& reason) {
    sun::support::logAndThrowError("Dependency '" + bundle + "', contract " +
                                   contract.compatibility_hash() + ": " +
                                   reason);
  };
  std::string verifierErrors;
  llvm::raw_string_ostream verifierStream(verifierErrors);
  if (llvm::verifyModule(module, &verifierStream))
    fail("invalid LLVM payload: " + verifierErrors);

  const auto actual = describeContract(module);
  validateContract(actual, bundle, targetTriple);
  const std::set<std::string> features(contract.target().cpu_features().begin(),
                                       contract.target().cpu_features().end());
  for (const auto& feature : actual.target().cpu_features())
    if (!features.count(feature))
      fail("emitted CPU feature '" + feature + "' missing from contract");
  std::map<std::string, std::string> requirements;
  for (const auto& rule : contract.requirements())
    requirements.emplace(rule.name(), rule.implementation());
  for (const auto& rule : actual.requirements()) {
    const auto found = requirements.find(rule.name());
    if (found == requirements.end() || found->second != rule.implementation())
      fail("emitted requirement '" + rule.name() +
           "' missing or different in contract");
  }
  std::set<std::string> expected(contract.boundary_records().begin(),
                                 contract.boundary_records().end());
  for (const auto& record : actual.boundary_records())
    if (!expected.count(record))
      sun::support::logAndThrowError(
          "Dependency '" + bundle + "', contract " +
          contract.compatibility_hash() +
          ": emitted layout or signature missing from contract");
}

void AbiCompatibilityChecker::recordValidatedImport(
    const BundleContract& contract, const std::string& bytesHash,
    const std::string& bundle) {
  if (importedContracts_.count(bytesHash)) return;
  try {
    auto combined = contract;
    for (const auto& [identity, imported] : importedContracts_)
      mergeContractRequirements(combined, imported);
  } catch (const sun::support::SunError& error) {
    sun::support::logAndThrowError(
        "Dependency '" + bundle + "', contract " +
        contract.compatibility_hash() +
        ": incompatible imported requirements: " + error.what());
  }
  importedContracts_.emplace(bytesHash, contract);
  const auto current = currentProducerIdentity();
  const auto ordering = compareProducerIdentity(contract.producer(), current);
  if ((ordering == ProducerOrdering::Older ||
       ordering == ProducerOrdering::Unknown) &&
      warnedBundles_.insert(bytesHash).second)
    sun::support::messageStream("warning")
        << "dependency " << bundle
        << (ordering == ProducerOrdering::Older
                ? " was built by an older producer"
                : " has unknown producer ordering")
        << "\n  producer: " << contract.producer().build()
        << "\n  consumer: " << current.build()
        << "\n  bundle contract verified; continuing\n";
}
}  // namespace sun::abi
