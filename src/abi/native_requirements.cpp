#include "abi/native_requirements.h"

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ELFObjectFile.h>

#include <set>

#include "abi/support_inventory.h"
#include "support/error.h"

/** Checks native archive structure and referenced runtime services. */
namespace sun::abi {
/** Keeps native object and runtime checks private to this translation unit. */
namespace {
/** Reports an unsupported native object or runtime requirement. */
[[noreturn]] void unsupported(const std::string& reason) {
  sun::support::logAndThrowError(
      "Native archive compatibility cannot be established: " + reason);
}
}  // namespace

std::string nativeArchiveRule(const TargetProfile& profile) {
  if ((profile.architecture() != "x86_64" &&
       profile.architecture() != "aarch64") ||
      (profile.platform_abi() != "linux:gnu" &&
       profile.platform_abi() != "linux:musl") ||
      profile.pointer_bits() != 64 || !profile.little_endian())
    unsupported("expected a Linux x86-64 or AArch64 GNU/musl profile");
  return "native.elf." + profile.architecture() + ".declared";
}

void addNativeRequirements(BundleContract& contract,
                           const std::vector<std::string>& cpuFeatures,
                           const std::vector<std::string>& runtimeServices) {
  if (cpuFeatures.empty() && runtimeServices.empty()) return;
  const auto nativeRule = nativeArchiveRule(contract.target());
  const auto triple =
      contract.target().architecture() + "-unknown-linux-" +
      (contract.target().platform_abi() == "linux:musl" ? "musl" : "gnu");
  compatibilityTarget(triple);
  std::string error;
  const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) unsupported(error);
  std::unique_ptr<llvm::MCSubtargetInfo> subtarget(
      target->createMCSubtargetInfo(triple, "generic", ""));
  if (!subtarget) unsupported("cannot determine CPU feature names");
  std::set<std::string> knownFeatures;
  for (const auto& feature : subtarget->getAllProcessorFeatures())
    knownFeatures.insert(feature.Key);
  auto declared = contract;
  for (const auto& feature : cpuFeatures) {
    if (!knownFeatures.count(feature))
      unsupported("unknown declared CPU feature '" + feature + "'");
    declared.mutable_target()->add_cpu_features(feature);
  }
  std::set<std::string> names{nativeRule};
  for (const auto& service : runtimeServices)
    names.insert("native.runtime." + service);
  const auto supported = supportedRules();
  for (const auto& name : names) {
    if (!supported.count(name))
      unsupported("unknown declared runtime rule '" + name + "'");
    bool present = false;
    for (const auto& rule : declared.requirements())
      if (rule.name() == name) present = true;
    if (!present) {
      auto* rule = declared.add_requirements();
      rule->set_name(name);
      rule->set_implementation(emissionImplementation(name));
    }
  }
  mergeContractRequirements(contract, declared);
}

void validateNativeArchives(const std::vector<NativeArchiveInput>& archives,
                            const TargetProfile& profile,
                            std::set<std::string>* runtimeServices,
                            std::set<std::string>* linkSymbols) {
  if (archives.empty()) return;
  nativeArchiveRule(profile);
  const auto machine = profile.architecture() == "x86_64"
                           ? llvm::ELF::EM_X86_64
                           : llvm::ELF::EM_AARCH64;
  std::set<std::string> definitions, references;
  for (const auto& input : archives) {
    auto archive = llvm::object::Archive::create(input.bytes);
    if (!archive) unsupported(llvm::toString(archive.takeError()));
    if ((*archive)->isThin())
      unsupported("thin archives require external member files");
    auto iterationError = llvm::Error::success();
    for (const auto& child : (*archive)->children(iterationError)) {
      auto binary = child.getAsBinary();
      if (!binary) unsupported(llvm::toString(binary.takeError()));
      const auto* object =
          llvm::dyn_cast<llvm::object::ELFObjectFileBase>(binary->get());
      if (!object || object->getEMachine() != machine ||
          object->getEType() != llvm::ELF::ET_REL ||
          !object->isLittleEndian() || object->getBytesInAddress() != 8)
        unsupported("expected a relocatable little-endian " +
                    profile.architecture() + " ELF member");
      for (const auto& section : object->sections()) {
        llvm::object::ELFSectionRef elfSection(section);
        if ((elfSection.getFlags() & llvm::ELF::SHF_TLS) && runtimeServices)
          runtimeServices->insert("thread_local");
        if (elfSection.getType() == llvm::ELF::SHT_INIT_ARRAY ||
            elfSection.getType() == llvm::ELF::SHT_FINI_ARRAY ||
            elfSection.getType() == llvm::ELF::SHT_PREINIT_ARRAY) {
          if (runtimeServices) runtimeServices->insert("initializers");
        }
        if (elfSection.getType() != llvm::ELF::SHT_NOBITS) {
          auto contents = section.getContents();
          if (!contents) unsupported(llvm::toString(contents.takeError()));
        }
      }
      for (const auto& symbol : object->symbols()) {
        auto flags = symbol.getFlags();
        auto name = symbol.getName();
        if (!flags) unsupported(llvm::toString(flags.takeError()));
        if (!name) unsupported(llvm::toString(name.takeError()));
        if (!(*flags & llvm::object::BasicSymbolRef::SF_Global) ||
            name->empty())
          continue;
        if (*flags & llvm::object::BasicSymbolRef::SF_Undefined)
          references.insert(name->str());
        else {
          if (!name->starts_with(input.symbolPrefix))
            unsupported("native definition lacks its bundle namespace");
          definitions.insert(name->str());
        }
      }
    }
    if (iterationError) unsupported(llvm::toString(std::move(iterationError)));
  }
  for (const auto& name : references) {
    if (definitions.count(name)) continue;
    // The ELF linker supplies the GOT base; it is not a runtime service.
    if (name == "_GLOBAL_OFFSET_TABLE_") continue;
    // Sun's JIT owns callback registration and drains it before unloading code;
    // native executables use the C runtime's corresponding process-exit
    // service.
    if (runtimeServices) {
      runtimeServices->insert("linker");
      if (name == "atexit") runtimeServices->insert("atexit");
    }
    if (linkSymbols) linkSymbols->insert(name);
  }
}
}  // namespace sun::abi
