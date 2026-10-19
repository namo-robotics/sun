#include "abi/native_requirements.h"

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <mutex>
#include <set>

#include "support/error.h"

/** Reconstructs supported platform and runtime requirements from native payloads. */
namespace sun::abi {
/** Keeps the initial native profile deliberately narrower than general native linking. */
namespace {
/** Reports a native requirement that the consumer cannot independently establish. */
[[noreturn]] void unsupported(const std::string& reason) {
  sun::support::logAndThrowError("Native archive compatibility cannot be established: " + reason);
}
/** Supports only audited baseline integer operations, with no implicit runtime service. */
bool baseline(llvm::StringRef opcode) {
  static const std::set<std::string> supported{
      "PUSH64r", "POP64r", "RET64", "NOOP", "NOOPL", "NOOPW",
      "MOV8rr", "MOV8rm", "MOV8mr", "MOV8ri", "MOV8mi",
      "MOV16rr", "MOV16rm", "MOV16mr", "MOV16ri", "MOV16mi",
      "MOV32rr", "MOV32rm", "MOV32mr", "MOV32ri", "MOV32mi",
      "MOV64rr", "MOV64rm", "MOV64mr", "MOV64ri", "MOV64ri32", "MOV64mi32",
      "MOVZX32rm8", "MOVZX32rm16", "MOVSX64rm32", "MOVSX64rr32",
      "LEA64r", "LEA32r", "ADD32ri", "ADD32ri8", "ADD32rr", "ADD32rm",
      "ADD64ri32", "ADD64ri8", "ADD64rr", "SUB32ri", "SUB32ri8", "SUB32rr",
      "SUB64ri32", "SUB64ri8", "SUB64rr", "XOR32rr", "XOR64rr",
      "AND32rr", "OR32rr", "CMP32rr", "CMP32ri", "CMP32ri8", "CMP32rm",
      "CMP64rr", "CMP64ri32", "CMP64ri8", "TEST32rr", "TEST64rr",
      "JMP_1", "JMP_4", "JCC_1", "JCC_4", "CALL64pcrel32", "LEAVE64"};
  return supported.count(opcode.str());
}
/** Allows relocation forms whose interpretation is supplied by the ELF x86-64 linker. */
bool relocation(uint64_t type) {
  switch (type) {
    case llvm::ELF::R_X86_64_NONE:
    case llvm::ELF::R_X86_64_64:
    case llvm::ELF::R_X86_64_PC32:
    case llvm::ELF::R_X86_64_PLT32:
    case llvm::ELF::R_X86_64_32:
    case llvm::ELF::R_X86_64_32S: return true;
    default: return false;
  }
}
}

void validateNativeArchives(const std::vector<NativeArchiveInput>& archives,
                            const TargetProfile& profile,
                            std::set<std::string>* runtimeServices) {
  if (archives.empty()) return;
  if (profile.architecture() != "x86_64" || profile.platform_abi() != "linux:gnu" ||
      profile.pointer_bits() != 64 || !profile.little_endian())
    unsupported("only the Linux x86-64 GNU profile is implemented");
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllDisassemblers();
  });
  const std::string triple = "x86_64-linux-gnu";
  std::string error;
  const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) unsupported(error);
  std::unique_ptr<llvm::MCRegisterInfo> registers(target->createMCRegInfo(triple));
  std::unique_ptr<llvm::MCSubtargetInfo> subtarget(target->createMCSubtargetInfo(triple, "x86-64", ""));
  std::unique_ptr<llvm::MCInstrInfo> instructions(target->createMCInstrInfo());
  std::unique_ptr<llvm::MCAsmInfo> assembly(target->createMCAsmInfo(*registers, triple, {}));
  llvm::MCContext context(llvm::Triple(triple), assembly.get(), registers.get(), subtarget.get());
  std::unique_ptr<llvm::MCDisassembler> decoder(target->createMCDisassembler(*subtarget, context));
  if (!decoder) unsupported("missing native instruction decoder");
  std::set<std::string> definitions, references;
  for (const auto& input : archives) {
    auto archive = llvm::object::Archive::create(input.bytes);
    if (!archive) unsupported(llvm::toString(archive.takeError()));
    if ((*archive)->isThin()) unsupported("thin archives require external member files");
    auto iterationError = llvm::Error::success();
    for (const auto& child : (*archive)->children(iterationError)) {
      auto binary = child.getAsBinary();
      if (!binary) unsupported(llvm::toString(binary.takeError()));
      const auto* object = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(binary->get());
      if (!object || object->getEMachine() != llvm::ELF::EM_X86_64 ||
          object->getEType() != llvm::ELF::ET_REL || !object->isLittleEndian() ||
          object->getBytesInAddress() != 8)
        unsupported("expected a relocatable little-endian x86-64 ELF member");
      for (const auto& section : object->sections()) {
        llvm::object::ELFSectionRef elfSection(section);
        if (elfSection.getFlags() & llvm::ELF::SHF_TLS)
          unsupported("thread-local native state is not implemented");
        if (elfSection.getType() == llvm::ELF::SHT_INIT_ARRAY ||
            elfSection.getType() == llvm::ELF::SHT_FINI_ARRAY ||
            elfSection.getType() == llvm::ELF::SHT_PREINIT_ARRAY)
          unsupported("native initialization and finalization are not implemented");
        for (const auto& fixup : section.relocations())
          if (!relocation(fixup.getType())) unsupported("unsupported native relocation");
        if (!(elfSection.getFlags() & llvm::ELF::SHF_EXECINSTR)) continue;
        auto code = section.getContents();
        if (!code) unsupported(llvm::toString(code.takeError()));
        uint64_t offset = 0;
        while (offset < code->size()) {
          llvm::MCInst instruction;
          uint64_t size = 0;
          const auto bytes = llvm::ArrayRef<uint8_t>(
              reinterpret_cast<const uint8_t*>(code->data() + offset), code->size() - offset);
          if (decoder->getInstruction(instruction, size, bytes, offset, llvm::nulls()) !=
                  llvm::MCDisassembler::Success || size == 0)
            unsupported("undecodable native instruction");
          const auto opcode = instructions->getName(instruction.getOpcode());
          if (!baseline(opcode)) unsupported("unsupported native instruction '" + opcode.str() + "'");
          offset += size;
        }
      }
      for (const auto& symbol : object->symbols()) {
        auto flags = symbol.getFlags();
        auto name = symbol.getName();
        if (!flags) unsupported(llvm::toString(flags.takeError()));
        if (!name) unsupported(llvm::toString(name.takeError()));
        if (!(*flags & llvm::object::BasicSymbolRef::SF_Global) || name->empty()) continue;
        if (*flags & llvm::object::BasicSymbolRef::SF_Undefined) references.insert(name->str());
        else {
          if (!name->starts_with(input.symbolPrefix)) unsupported("native definition lacks its bundle namespace");
          definitions.insert(name->str());
        }
      }
    }
    if (iterationError) unsupported(llvm::toString(std::move(iterationError)));
  }
  for (const auto& name : references) {
    if (definitions.count(name)) continue;
    // Sun's JIT owns callback registration and drains it before unloading code;
    // native executables use the C runtime's corresponding process-exit service.
    if (name == "atexit") {
      if (runtimeServices) runtimeServices->insert("atexit");
    } else unsupported("unestablished runtime symbol '" + name + "'");
  }
}
}
