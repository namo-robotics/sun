/** Load LLVM Mach-O declarations before Protobuf imports Apple Mach macros. */
#include <llvm/Object/MachO.h>

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
/** Recognizes supported desktop Apple target profiles, including OS versions.
 */
bool isMacOS(const TargetProfile& profile) {
  const auto& abi = profile.platform_abi();
  return (abi.starts_with("darwin") || abi.starts_with("macos")) &&
         abi.ends_with(":");
}
}  // namespace

std::string nativeArchiveRule(const TargetProfile& profile) {
  if ((profile.architecture() != "x86_64" &&
       profile.architecture() != "aarch64") ||
      (!isMacOS(profile) && profile.platform_abi() != "linux:gnu" &&
       profile.platform_abi() != "linux:musl") ||
      profile.pointer_bits() != 64 || !profile.little_endian())
    unsupported("expected a Linux GNU/musl or macOS x86-64/AArch64 profile");
  return std::string(isMacOS(profile) ? "native.macho." : "native.elf.") +
         profile.architecture() + ".declared";
}

void addNativeRequirements(BundleContract& contract,
                           const std::vector<std::string>& cpuFeatures,
                           const std::vector<std::string>& runtimeServices) {
  if (cpuFeatures.empty() && runtimeServices.empty()) return;
  const auto nativeRule = nativeArchiveRule(contract.target());
  const auto triple = contract.target().architecture() +
                      (isMacOS(contract.target()) ? "-apple-darwin"
                       : contract.target().platform_abi() == "linux:musl"
                           ? "-unknown-linux-musl"
                           : "-unknown-linux-gnu");
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
          llvm::dyn_cast<llvm::object::ObjectFile>(binary->get());
      if (!object || !object->isLittleEndian() ||
          object->getBytesInAddress() != 8)
        unsupported("expected a 64-bit little-endian object member");
      const auto* elf = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(object);
      const auto* macho = llvm::dyn_cast<llvm::object::MachOObjectFile>(object);
      const bool matches =
          isMacOS(profile)
              ? macho &&
                    macho->getHeader64().filetype == llvm::MachO::MH_OBJECT &&
                    macho->getArch() ==
                        (profile.architecture() == "x86_64"
                             ? llvm::Triple::x86_64
                             : llvm::Triple::aarch64)
              : elf && elf->getEMachine() == machine &&
                    elf->getEType() == llvm::ELF::ET_REL;
      if (!matches)
        unsupported("expected a relocatable little-endian " +
                    profile.architecture() +
                    (isMacOS(profile) ? " Mach-O member" : " ELF member"));
      for (const auto& section : object->sections()) {
        if (elf) {
          llvm::object::ELFSectionRef elfSection(section);
          if ((elfSection.getFlags() & llvm::ELF::SHF_TLS) && runtimeServices)
            runtimeServices->insert("thread_local");
          if (runtimeServices &&
              (elfSection.getType() == llvm::ELF::SHT_INIT_ARRAY ||
               elfSection.getType() == llvm::ELF::SHT_FINI_ARRAY ||
               elfSection.getType() == llvm::ELF::SHT_PREINIT_ARRAY))
            runtimeServices->insert("initializers");
        } else {
          const auto type =
              macho->getSection64(section.getRawDataRefImpl()).flags &
              llvm::MachO::SECTION_TYPE;
          if (runtimeServices) {
            if (type >= llvm::MachO::S_THREAD_LOCAL_REGULAR &&
                type <= llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS)
              runtimeServices->insert("thread_local");
            if (type == llvm::MachO::S_MOD_INIT_FUNC_POINTERS ||
                type == llvm::MachO::S_MOD_TERM_FUNC_POINTERS)
              runtimeServices->insert("initializers");
          }
        }
        if (!section.isVirtual()) {
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
        // Mach-O adds an assembler underscore to every external C name.
        if (macho && name->starts_with("_")) *name = name->drop_front();
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
