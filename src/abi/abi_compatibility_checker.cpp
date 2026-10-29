#include "abi/abi_compatibility_checker.h"

#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/Error.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <optional>

#include "abi/abi_validation_scope.h"
#include "abi/boundary_requirements.h"
#include "abi/library_abi_snapshot.h"
#include "abi/native_requirements.h"
#include "abi/nominal_identity.h"
#include "abi/producer_identity.h"
#include "abi/semantic_requirements.h"
#include "abi/support_inventory.h"
#include "moon_bundling/moon.h"
#include "support/error.h"
#include "support/terminal.h"
#include "support/terminal_format.h"

/** Owns compatibility policy independently of bundle container decoding. */
namespace sun::abi {

/** Formats diagnostic branches from validated bundle requirements. */
namespace {
/** Shows every import retaining an artifact and the selected compatible import.
 * Embedded requirements include transitive dependencies; intermediate edges are
 * not retained in the bundle contract.
 */
std::string artifactDependencyTree(
    const proto::LibraryAbiSnapshot& required,
    const proto::LibraryAbiSnapshot& selected,
    const std::vector<moon_bundling::MoonReader*>& readers,
    const std::string& entrypoint) {
  std::set<std::string> embeddedPaths;
  for (const auto* reader : readers) {
    for (const auto& dependency :
         reader->getLibrary().bundle_contract().dependency_abis()) {
      if (dependency.library_id() == required.library_id() &&
          dependency.artifact_hash() == required.artifact_hash()) {
        embeddedPaths.insert(reader->getPath().string());
      }
    }
  }
  auto& stream = llvm::errs();
  sun::support::TerminalTreeNode root{
      entrypoint.empty() ? "Current compilation"
                         : sun::support::terminalFileLabel(entrypoint, stream),
      {},
      {}};
  for (const auto& path : embeddedPaths) {
    root.children.push_back({sun::support::terminalFileLabel(path, stream),
                             {},
                             {{required.library_id(),
                               {"artifact: " + required.artifact_hash()},
                               {}}}});
  }
  root.children.push_back({selected.library_id() + " [selected import]",
                           {"artifact: " + selected.artifact_hash()},
                           {}});
  const auto tree =
      "\n" + sun::support::terminalTree(root, stream, "warning", "  ");
  return tree;
}
}  // namespace

AbiCompatibilityChecker::AbiCompatibilityChecker(std::string targetTriple,
                                                 NativeExecution execution)
    : AbiCompatibilityChecker(
          CompilationContext{std::move(targetTriple), execution}) {}

AbiCompatibilityChecker::AbiCompatibilityChecker(CompilationContext context)
    : context_(std::move(context)) {}

AbiCompatibilityChecker& AbiCompatibilityChecker::current() {
  thread_local AbiCompatibilityChecker checker;
  return AbiValidationScope::currentChecker_
             ? *AbiValidationScope::currentChecker_
             : checker;
}

bool AbiCompatibilityChecker::hasValidationScope() {
  return AbiValidationScope::currentChecker_ != nullptr;
}

void AbiCompatibilityChecker::prepareCompilation(
    const std::string& targetTriple, NativeExecution execution, bool debugInfo,
    bool optimize) {
  auto& checker = current();
  const auto& context = checker.context();
  if (AbiValidationScope::currentChecker_ &&
      context.targetTriple == targetTriple && context.execution == execution &&
      context.debugInfo == debugInfo && context.optimize == optimize) {
    return;
  }
  checker.beginCompilation(targetTriple, execution);
  checker.context_.debugInfo = debugInfo;
  checker.context_.optimize = optimize;
}

void AbiCompatibilityChecker::configure(CompilationContext context) {
  if (context_ == context) {
    return;
  }
  beginCompilation(context.targetTriple, context.execution);
  context_ = std::move(context);
}

void AbiCompatibilityChecker::beginCompilation(const std::string& targetTriple,
                                               NativeExecution execution) {
  context_ = CompilationContext{targetTriple, execution};
  byteHashes_.clear();
  warnedBundles_.clear();
  warnedArtifactPairs_.clear();
  importedContracts_.clear();
  declarationAliases_.clear();
  validatedBundles_.clear();
}

std::string AbiCompatibilityChecker::bytesHash(
    const moon_bundling::MoonReader& reader) const {
  const std::weak_ptr<const std::string> snapshot(reader.getBytes());
  if (auto found = byteHashes_.find(snapshot); found != byteHashes_.end()) {
    return found->second;
  }
  return byteHashes_
      .emplace(snapshot, moon_bundling::computeSha256Hex(*reader.getBytes()))
      .first->second;
}

ValidatedBundle AbiCompatibilityChecker::validate(
    moon_bundling::MoonReader& reader) {
  return validate(reader, context_.targetTriple);
}

ValidatedBundle AbiCompatibilityChecker::validate(
    moon_bundling::MoonReader& reader, const std::string& targetTriple) {
  return validateImpl(reader, targetTriple, true);
}

void AbiCompatibilityChecker::validateImports(
    const std::vector<moon_bundling::MoonReader*>& readers,
    const std::string& entrypoint) {
  std::vector<const moon::ModuleMetadata*> metadata;
  std::set<std::string> snapshots;
  std::string imports;
  std::optional<BundleContract> combined;
  for (const auto& [identity, contract] : importedContracts_) {
    if (combined) {
      mergeContractRequirements(*combined, contract);
    } else {
      combined = contract;
    }
  }
  for (auto* reader : readers) {
    const auto validated = validateImpl(*reader, context_.targetTriple, false);
    imports += " '" + reader->getPath().string() + "' (contract " +
               validated.contract().compatibility_hash() + ")";
    if (!snapshots.insert(bytesHash(*reader)).second) {
      continue;
    }
    if (combined) {
      mergeContractRequirements(*combined, validated.contract());
    } else {
      combined = validated.contract();
    }
    for (const auto& key : reader->listModules()) {
      metadata.push_back(reader->getMetadata(key));
    }
  }
  std::map<std::string, std::map<std::string, const proto::LibraryAbiSnapshot*>>
      candidates;
  std::set<std::string> exactArtifacts;
  std::map<std::string, std::string> aliases;
  std::map<std::pair<std::string, std::string>, std::string> artifactWarnings;
  for (const auto* reader : readers) {
    const auto& library = reader->getLibrary();
    exactArtifacts.insert(library.content_hash());
    const auto& contract = library.bundle_contract();
    if (!contract.has_library_abi()) {
      continue;
    }
    const auto& revision = contract.library_abi();
    candidates[revision.library_id()].emplace(revision.artifact_hash(),
                                              &revision);
  }
  for (const auto* reader : readers) {
    for (const auto& required :
         reader->getLibrary().bundle_contract().dependency_abis()) {
      if (exactArtifacts.count(required.artifact_hash())) {
        continue;
      }
      const auto found = candidates.find(required.library_id());
      if (found == candidates.end()) {
        continue;  // Exact-reference validation diagnoses missing imports.
      }
      if (found->second.size() != 1) {
        sun::support::logAndThrowError("Ambiguous library revisions for '" +
                                       required.library_id() + "'");
      }
      const auto& candidate = *found->second.begin()->second;
      if (required.abi_hash() != candidate.abi_hash()) {
        sun::support::logAndThrowError("Library ABI mismatch for '" +
                                       required.library_id() +
                                       "': selected artifact changes the "
                                       "required declarations, target, or "
                                       "runtime contract");
      }
      artifactWarnings.emplace(
          std::minmax(required.artifact_hash(), candidate.artifact_hash()),
          "Library '" + required.library_id() +
              "' is used through different artifacts." +
              artifactDependencyTree(required, candidate, readers, entrypoint));
      std::map<std::string, std::string> keys;
      for (const auto& item : candidate.declarations()) {
        if (!keys.emplace(item.identity(), item.key()).second) {
          sun::support::logAndThrowError("Ambiguous replacement declaration");
        }
      }
      for (const auto& item : required.declarations()) {
        const auto replacement = keys.find(item.identity());
        if (replacement == keys.end()) {
          sun::support::logAndThrowError(
              "Missing replacement nominal declaration");
        }
        const auto [it, added] =
            aliases.emplace(item.key(), replacement->second);
        if (!added && it->second != replacement->second) {
          sun::support::logAndThrowError(
              "Conflicting replacement nominal declaration");
        }
      }
    }
  }
  std::vector<moon::ModuleMetadata> normalized;
  normalized.reserve(metadata.size());
  for (const auto* module : metadata) {
    normalized.push_back(*module);
    remapDeclarationReferences(normalized.back(), aliases);
  }
  std::vector<const moon::ModuleMetadata*> correspondence;
  for (const auto& module : normalized) {
    correspondence.push_back(&module);
  }
  try {
    validateDeclarationGraph(correspondence, true);
  } catch (const sun::support::SunError& error) {
    sun::support::logAndThrowError("Invalid dependency declaration graph for" +
                                   imports + ": " + error.what());
  }
  declarationAliases_ = std::move(aliases);
  for (auto* reader : readers) {
    recordValidatedImport(reader->getLibrary().bundle_contract(),
                          bytesHash(*reader), reader->getPath().string());
  }
  for (const auto& [artifacts, warning] : artifactWarnings) {
    if (warnedArtifactPairs_.insert(artifacts).second) {
      sun::support::messageStream("warning") << warning << '\n';
    }
  }
}

ValidatedBundle AbiCompatibilityChecker::validateImpl(
    moon_bundling::MoonReader& reader, const std::string& targetTriple,
    bool record) {
  const auto digest = bytesHash(reader);
  const auto key = std::make_pair(
      digest, targetTriple + ":" +
                  std::to_string(static_cast<int>(context_.execution)) + ":" +
                  compatibilityImplementation());
  auto found = validatedBundles_.find(key);
  if (found == validatedBundles_.end()) {
    const auto modules = reader.listModules();
    const auto* metadata = &reader.getLibrary();
    if (!metadata || !metadata->has_bundle_contract()) {
      sun::support::logAndThrowError(
          "Dependency '" + reader.getPath().string() +
          "': compatibility cannot be established; missing bundle contract; "
          "rebuild the bundle");
    }
    const auto& contract = metadata->bundle_contract();
    validateContract(contract, reader.getPath().string(), targetTriple);
    const std::set<std::string> semantics(contract.semantic_records().begin(),
                                          contract.semantic_records().end());
    bool metadataRule = false;
    for (const auto& rule : contract.requirements()) {
      if (rule.name() == "moon.metadata") {
        metadataRule = true;
      }
    }
    if (!metadataRule) {
      sun::support::logAndThrowError(
          "Dependency '" + reader.getPath().string() + "', contract " +
          contract.compatibility_hash() +
          ": missing moon.metadata requirement");
    }
    std::vector<const moon::ModuleMetadata*> declarationModules;
    std::map<std::string, std::vector<const moon::ModuleMetadata*>>
        payloadMetadata;
    for (const auto& moduleKey : modules) {
      const auto* metadata = reader.getMetadata(moduleKey);
      declarationModules.push_back(metadata);
      payloadMetadata[reader.getBitcodeId(moduleKey)].push_back(metadata);
    }
    try {
      validateDeclarationGraph(declarationModules);
    } catch (const sun::support::SunError& error) {
      sun::support::logAndThrowError(
          "Dependency '" + reader.getPath().string() + "', contract " +
          contract.compatibility_hash() + ": " + error.what());
    }
    if (contract.has_library_abi()) {
      if (!contract.has_library_id() ||
          encodeLibraryAbiSnapshot(contract.library_abi()) !=
              encodeLibraryAbiSnapshot(describeLibraryAbiSnapshot(
                  contract, reader.getLibrary().content_hash(),
                  declarationModules))) {
        sun::support::logAndThrowError(
            "Library revision differs from its declarations");
      }
    }
    std::set<std::string> checked;
    for (const auto& moduleKey : modules) {
      try {
        if (!semantics.count(
                describeSemantics(*reader.getMetadata(moduleKey)))) {
          sun::support::logAndThrowError(
              "declarations or retained bodies differ from contract");
        }
      } catch (const sun::support::SunError& error) {
        sun::support::logAndThrowError(
            "Dependency '" + reader.getPath().string() + "', contract " +
            contract.compatibility_hash() + ": " + error.what());
      }
      if (!checked.insert(reader.getBitcodeId(moduleKey)).second) {
        continue;
      }
      llvm::LLVMContext context;
      auto module = reader.loadModule(moduleKey, context);
      if (!module) {
        sun::support::logAndThrowError(
            "Unsupported Moon payload in '" + reader.getPath().string() +
            "', contract " + contract.compatibility_hash() + ": " +
            reader.getError());
      }
      if (auto error = module->materializeAll()) {
        sun::support::logAndThrowError(
            "Unsupported Moon payload in '" + reader.getPath().string() +
            "', contract " + contract.compatibility_hash() + ": " +
            llvm::toString(std::move(error)));
      }
      validatePayload(contract, *module, reader.getPath().string(),
                      targetTriple);
      try {
        validatePayloadDeclarations(
            payloadMetadata.at(reader.getBitcodeId(moduleKey)), *module);
        for (const auto* metadata :
             payloadMetadata.at(reader.getBitcodeId(moduleKey))) {
          validateSpecializationPayload(*metadata, *module);
        }
      } catch (const sun::support::SunError& error) {
        sun::support::logAndThrowError(
            "Dependency '" + reader.getPath().string() + "', contract " +
            contract.compatibility_hash() + ": " + error.what());
      }
    }
    if (!reader.getNativeArchives().empty()) {
      bool nativeRule = false;
      for (const auto& rule : contract.requirements()) {
        if (rule.name() == nativeArchiveRule(contract.target())) {
          nativeRule = true;
        }
      }
      try {
        if (!nativeRule) {
          sun::support::logAndThrowError("missing native archive requirements");
        }
        std::vector<std::vector<char>> buffers(
            reader.getNativeArchives().size());
        std::vector<NativeArchiveInput> archives;
        size_t index = 0;
        for (const auto& entry : reader.getNativeArchives()) {
          auto& bytes = buffers[index++];
          if (!reader.readNativeArchive(entry, bytes)) {
            sun::support::logAndThrowError(reader.getError());
          }
          archives.push_back(
              {llvm::MemoryBufferRef(
                   llvm::StringRef(bytes.data(), bytes.size()), entry.name),
               "$" + entry.archiveSetHash + "$_"});
        }
        std::set<std::string> runtimeServices;
        validateNativeArchives(archives, contract.target(), &runtimeServices);
        for (const auto& service : runtimeServices) {
          bool recorded = false;
          for (const auto& rule : contract.requirements()) {
            if (rule.name() == "native.runtime." + service) {
              recorded = true;
            }
          }
          if (!recorded) {
            sun::support::logAndThrowError(
                "missing native runtime requirement: " + service);
          }
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
  if (record) {
    recordValidatedImport(*found->second, digest, reader.getPath().string());
  }
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
  if (contract.encoding_schema() != 1 || !contract.experimental()) {
    fail("unsupported contract encoding or stability claim");
  }
  if (contract.has_library_id()) {
    try {
      validateLibraryId(contract.library_id());
    } catch (const sun::support::SunError& error) {
      fail(error.what());
    }
  }
  if (hashBundleContract(contract) != contract.compatibility_hash()) {
    fail("contract description does not match its hash");
  }
  auto expected = compatibilityTarget(targetTriple);
  auto actual = contract.target();
  actual.clear_cpu_features();
  if (expected.architecture() != actual.architecture() ||
      expected.platform_abi() != actual.platform_abi() ||
      expected.data_layout() != actual.data_layout() ||
      expected.pointer_bits() != actual.pointer_bits() ||
      expected.little_endian() != actual.little_endian()) {
    fail("incompatible target profile for current target '" + targetTriple +
         "'");
  }
  // Native code is emitted for the generic destination CPU. Only JIT
  // execution may rely on capabilities of the compiler host.
  llvm::StringMap<bool> availableFeatures;
  const bool nativeArchitecture =
      expected.architecture() ==
      llvm::Triple::getArchTypeName(
          llvm::Triple(llvm::sys::getDefaultTargetTriple()).getArch())
          .str();
  const bool hostExecution =
      nativeArchitecture && context_.execution == NativeExecution::JIT;
  if (hostExecution) {
    availableFeatures = llvm::sys::getHostCPUFeatures();
  }
  // LLVM does not report host features on every platform (including Apple
  // ARM). Use its CPU model when probing is unavailable. An explicit false
  // feature from a successful probe must still be respected.
  if (availableFeatures.empty()) {
    const auto triple = llvm::Triple::normalize(
        targetTriple.empty() ? llvm::sys::getDefaultTargetTriple()
                             : targetTriple);
    std::string error;
    const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
      fail("cannot determine target CPU capabilities: " + error);
    }
    std::unique_ptr<llvm::MCSubtargetInfo> subtarget(
        target->createMCSubtargetInfo(
            triple, hostExecution ? llvm::sys::getHostCPUName() : "generic",
            ""));
    if (!subtarget) {
      fail("cannot determine target CPU capabilities");
    }
    for (const auto& feature : subtarget->getEnabledProcessorFeatures()) {
      availableFeatures[feature.Key] = true;
    }
  }
  for (const auto& feature : contract.target().cpu_features()) {
    if (!availableFeatures.lookup(feature)) {
      fail("unsupported CPU feature '" + feature + "' for selected target");
    }
    expected.add_cpu_features(feature);
  }
  BundleContract derived;
  derived.set_encoding_schema(1);
  if (contract.has_library_id()) {
    derived.set_library_id(contract.library_id());
  }
  derived.set_experimental(true);
  *derived.mutable_target() = expected;
  const auto supportedImplementations = supportedRules();
  std::set<std::string> seen;
  for (const auto& rule : contract.requirements()) {
    if (!seen.insert(rule.name()).second) {
      fail("duplicate required rule '" + rule.name() + "'");
    }
    const auto found = supportedImplementations.find(rule.name());
    if (found == supportedImplementations.end()) {
      fail("unknown required capability '" + rule.name() + "'");
    }
    const auto& implementation = found->second;
    if (rule.implementation() != implementation) {
      fail("unsupported implementation of '" + rule.name() + "'");
    }
    auto* supported = derived.add_requirements();
    supported->set_name(rule.name());
    supported->set_implementation(implementation);
  }
  for (const auto* mandatory :
       {"moon.interface", "llvm.bitcode", "sun.experimental.semantics"}) {
    if (!seen.count(mandatory)) {
      fail(std::string("missing required rule '") + mandatory + "'");
    }
  }
  if (context_.execution == NativeExecution::JIT &&
      (seen.count("native.runtime.initializers") ||
       seen.count("native.runtime.thread_local"))) {
    fail(
        "native initialization or thread-local storage requires native "
        "linking; the JIT does not implement it");
  }
  for (const auto& rule : contract.requirements()) {
    if ((rule.name().starts_with("native.elf.") ||
         rule.name().starts_with("native.macho.")) &&
        rule.name() != nativeArchiveRule(contract.target())) {
      fail("native object rule does not match the target");
    }
  }
  // Boundary records are checked against decoded payloads by this checker. The
  // exact semantics closure supplies interpretation of retained generic ASTs.
  BoundaryRequirements boundaries;
  try {
    for (const auto& record : contract.boundary_records()) {
      boundaries.add(record);
    }
  } catch (const sun::support::SunError& error) {
    fail(error.what());
  }
  for (const auto& record : boundaries.records()) {
    derived.add_boundary_records(record);
  }
  for (const auto& record : contract.semantic_records()) {
    derived.add_semantic_records(record);
  }
  /** Rejects malformed dependency metadata before it can authorize aliases. */
  const auto validateRevision = [&](const proto::LibraryAbiSnapshot& revision) {
    validateLibraryId(revision.library_id());
    for (const auto* digest :
         {&revision.artifact_hash(), &revision.abi_hash()}) {
      if (digest->size() != 64 ||
          digest->find_first_not_of("0123456789abcdef") != std::string::npos) {
        fail("invalid library revision digest");
      }
    }
    std::set<std::string> keys, identities;
    const auto prefix = "$" + revision.artifact_hash() + "$_";
    for (const auto& item : revision.declarations()) {
      const auto ordinal =
          item.key().substr(std::min(prefix.size(), item.key().size()));
      if (!item.key().starts_with(prefix) || ordinal.empty() ||
          ordinal.find_first_not_of("0123456789") != std::string::npos ||
          item.identity().empty() || !keys.insert(item.key()).second ||
          !identities.insert(item.identity()).second) {
        fail("invalid or ambiguous nominal declaration requirement");
      }
    }
  };
  if (contract.has_library_abi()) {
    validateRevision(contract.library_abi());
    *derived.mutable_library_abi() = contract.library_abi();
  }
  std::set<std::string> dependencyArtifacts;
  for (const auto& revision : contract.dependency_abis()) {
    validateRevision(revision);
    if (!dependencyArtifacts.insert(revision.artifact_hash()).second) {
      fail("duplicate library revision requirement");
    }
    *derived.add_dependency_abis() = revision;
  }
  if (hashBundleContract(derived) != contract.compatibility_hash()) {
    fail("derived contract hash differs");
  }
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
  if (llvm::verifyModule(module, &verifierStream)) {
    fail("invalid LLVM payload: " + verifierErrors);
  }

  const auto actual = describeContract(module);
  validateContract(actual, bundle, targetTriple);
  const std::set<std::string> features(contract.target().cpu_features().begin(),
                                       contract.target().cpu_features().end());
  for (const auto& feature : actual.target().cpu_features()) {
    if (!features.count(feature)) {
      fail("emitted CPU feature '" + feature + "' missing from contract");
    }
  }
  std::map<std::string, std::string> requirements;
  for (const auto& rule : contract.requirements()) {
    requirements.emplace(rule.name(), rule.implementation());
  }
  for (const auto& rule : actual.requirements()) {
    const auto found = requirements.find(rule.name());
    if (found == requirements.end() || found->second != rule.implementation()) {
      fail("emitted requirement '" + rule.name() +
           "' missing or different in contract");
    }
  }
  std::set<std::string> expected(contract.boundary_records().begin(),
                                 contract.boundary_records().end());
  for (const auto& record : actual.boundary_records()) {
    if (!expected.count(record)) {
      sun::support::logAndThrowError(
          "Dependency '" + bundle + "', contract " +
          contract.compatibility_hash() +
          ": emitted layout or signature missing from contract");
    }
  }
}

void AbiCompatibilityChecker::recordValidatedImport(
    const BundleContract& contract, const std::string& bytesHash,
    const std::string& bundle) {
  if (importedContracts_.count(bytesHash)) {
    return;
  }
  try {
    auto combined = contract;
    for (const auto& [identity, imported] : importedContracts_) {
      mergeContractRequirements(combined, imported);
    }
  } catch (const sun::support::SunError& error) {
    sun::support::logAndThrowError(
        "Dependency '" + bundle + "', contract " +
        contract.compatibility_hash() +
        ": incompatible imported requirements: " + error.what());
  }
  importedContracts_.emplace(bytesHash, contract);
  if (!context_.producerWarnings) {
    return;
  }
  const auto current = currentProducerIdentity();
  const auto& producerRuntime = contract.producer().runtime_fingerprint();
  if (!producerRuntime.empty() &&
      producerRuntime != current.runtime_fingerprint()) {
    sun::support::messageStream("warning")
        << "dependency " << bundle
        << " was built by Sun using different compiler shared libraries than "
           "the current Sun process"
        << "\n  This compares compiler environments, not your application's "
           "runtime dependencies."
        << "\n  ABI compatibility checks passed; continuing\n";
  }
  const auto ordering = compareProducerIdentity(contract.producer(), current);
  if ((ordering == ProducerOrdering::Older ||
       ordering == ProducerOrdering::Unknown) &&
      warnedBundles_.insert(bytesHash).second) {
    sun::support::messageStream("warning")
        << "dependency " << bundle
        << (ordering == ProducerOrdering::Older
                ? " was built by an older producer"
                : " has unknown producer ordering")
        << "\n  producer: " << contract.producer().build()
        << "\n  consumer: " << current.build()
        << "\n  bundle contract verified; continuing\n";
  }
}
}  // namespace sun::abi
