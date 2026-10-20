#include "abi/native_requirements.h"

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ELFObjectFile.h>

#include <set>

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

void validateNativeArchives(const std::vector<NativeArchiveInput>& archives,
                            const TargetProfile& profile,
                            std::set<std::string>* runtimeServices) {
  if (archives.empty()) return;
  if (profile.architecture() != "x86_64" ||
      profile.platform_abi() != "linux:gnu" || profile.pointer_bits() != 64 ||
      !profile.little_endian())
    unsupported("only the Linux x86-64 GNU profile is implemented");
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
      if (!object || object->getEMachine() != llvm::ELF::EM_X86_64 ||
          object->getEType() != llvm::ELF::ET_REL ||
          !object->isLittleEndian() || object->getBytesInAddress() != 8)
        unsupported("expected a relocatable little-endian x86-64 ELF member");
      for (const auto& section : object->sections()) {
        llvm::object::ELFSectionRef elfSection(section);
        if (elfSection.getFlags() & llvm::ELF::SHF_TLS)
          unsupported("thread-local native state is not implemented");
        if (elfSection.getType() == llvm::ELF::SHT_INIT_ARRAY ||
            elfSection.getType() == llvm::ELF::SHT_FINI_ARRAY ||
            elfSection.getType() == llvm::ELF::SHT_PREINIT_ARRAY)
          unsupported(
              "native initialization and finalization are not implemented");
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
    if (name == "atexit") {
      if (runtimeServices) runtimeServices->insert("atexit");
    } else
      unsupported("unestablished runtime symbol '" + name + "'");
  }
}
}  // namespace sun::abi
