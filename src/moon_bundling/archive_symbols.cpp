// archive_symbols.cpp — see archive_symbols.h

#include "moon_bundling/archive_symbols.h"

#include <llvm/ADT/StringMap.h>
#include <llvm/BinaryFormat/Magic.h>
#include <llvm/ObjCopy/ConfigManager.h>
#include <llvm/ObjCopy/ObjCopy.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/SymbolicFile.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/StringSaver.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <vector>

#include "moon_bundling/moon.h"

namespace sun {

namespace {

using llvm::object::Archive;
using llvm::object::BasicSymbolRef;
using llvm::object::SymbolicFile;

llvm::Error makeError(const std::string& message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), "%s",
                                 message.c_str());
}

// The name of an archive member, for messages
std::string memberName(const Archive::Child& child) {
  auto name = child.getName();
  if (!name) {
    llvm::consumeError(name.takeError());
    return "<unnamed member>";
  }
  return name->str();
}

// Object symbols carry the assembler's global prefix on Mach-O; Sun code and
// the `as "..."` names in extern declarations never do.
std::string bareName(llvm::StringRef objectName, bool machO) {
  if (machO && objectName.starts_with("_")) return objectName.drop_front().str();
  return objectName.str();
}

std::string objectName(const std::string& bare, bool machO) {
  return machO ? "_" + bare : bare;
}

// Open one member as an object with a symbol table. Bitcode members are
// refused by name: nothing below could rename their symbols.
llvm::Expected<std::unique_ptr<llvm::object::Binary>> openMember(
    const Archive::Child& child) {
  auto buffer = child.getMemoryBufferRef();
  if (!buffer) return buffer.takeError();
  if (llvm::identify_magic(buffer->getBuffer()) == llvm::file_magic::bitcode) {
    return makeError("member '" + memberName(child) +
                     "' is LLVM bitcode (an LTO build), not a native object");
  }
  auto binary = child.getAsBinary();
  if (!binary) return binary.takeError();
  if ((*binary)->isMachOUniversalBinary()) {
    return makeError("member '" + memberName(child) +
                     "' is a universal binary; carry one architecture");
  }
  if (!llvm::isa<SymbolicFile>(binary->get())) {
    return makeError("member '" + memberName(child) +
                     "' is not a native object file");
  }
  return binary;
}

// One pass over every member; `visit` sees each member as an object.
llvm::Error forEachMember(
    const Archive& archive,
    llvm::function_ref<llvm::Error(const Archive::Child&,
                                   llvm::object::Binary&)>
        visit) {
  llvm::Error err = llvm::Error::success();
  for (const Archive::Child& child : archive.children(err)) {
    auto binary = openMember(child);
    if (!binary) return binary.takeError();
    if (auto visitErr = visit(child, **binary)) return visitErr;
  }
  return err;
}

// `$hash$_name` -> `name`; anything else unchanged.
std::string stripBundlePrefix(const std::string& symbol) {
  if (symbol.size() < 3 || symbol[0] != '$') return symbol;
  size_t close = symbol.find('$', 1);
  if (close == std::string::npos || close + 1 >= symbol.size() ||
      symbol[close + 1] != '_') {
    return symbol;
  }
  return symbol.substr(close + 2);
}

}  // namespace

std::string computeArchiveSetHash(
    const std::vector<std::pair<std::string, std::string>>& namesAndDigests) {
  std::vector<std::string> lines;
  for (const auto& [name, digest] : namesAndDigests) {
    lines.push_back(name + ":" + digest + "\n");
  }
  std::sort(lines.begin(), lines.end());
  std::string input;
  for (const auto& line : lines) input += line;
  return computeSha256Hex(input).substr(0, 16);
}

llvm::Expected<ArchiveSymbolScan> scanArchiveSymbols(
    llvm::MemoryBufferRef archiveBytes) {
  auto archive = Archive::create(archiveBytes);
  if (!archive) return archive.takeError();

  ArchiveSymbolScan scan;
  bool machO = false;
  bool sawMember = false;
  auto err = forEachMember(
      **archive,
      [&](const Archive::Child& child,
          llvm::object::Binary& bin) -> llvm::Error {
        if (!sawMember) {
          machO = bin.isMachO();
          sawMember = true;
        } else if (bin.isMachO() != machO) {
          return makeError("member '" + memberName(child) +
                           "' uses a different object format than the rest");
        }
        auto& object = llvm::cast<SymbolicFile>(bin);
        for (const BasicSymbolRef& symbol : object.symbols()) {
          auto flags = symbol.getFlags();
          if (!flags) return flags.takeError();
          if (!(*flags & BasicSymbolRef::SF_Global)) continue;
          if (*flags & BasicSymbolRef::SF_FormatSpecific) continue;
          std::string name;
          llvm::raw_string_ostream nameStream(name);
          if (auto printErr = symbol.printName(nameStream)) return printErr;
          nameStream.flush();
          if (name.empty()) continue;
          if (*flags & BasicSymbolRef::SF_Undefined) {
            scan.undefined.insert(bareName(name, machO));
          } else {
            scan.defined.insert(bareName(name, machO));
          }
        }
        return llvm::Error::success();
      });
  if (err) return std::move(err);
  // A reference satisfied by another member is internal to the archive
  for (const auto& name : scan.defined) scan.undefined.erase(name);
  return scan;
}

llvm::Expected<std::string> renameArchiveSymbols(
    llvm::MemoryBufferRef archiveBytes,
    const std::map<std::string, std::string>& renames) {
  auto archive = Archive::create(archiveBytes);
  if (!archive) return archive.takeError();

  // The rename table is built once the object format is known, since the
  // spelling differs; objcopy keeps only references into it.
  llvm::BumpPtrAllocator allocator;
  llvm::StringSaver saver(allocator);
  llvm::objcopy::ConfigManager config;
  bool tableBuilt = false;

  std::vector<llvm::NewArchiveMember> members;
  auto err = forEachMember(
      **archive,
      [&](const Archive::Child& child,
          llvm::object::Binary& bin) -> llvm::Error {
        if (!tableBuilt) {
          const bool machO = bin.isMachO();
          for (const auto& [from, to] : renames) {
            config.Common.SymbolsToRename[saver.save(objectName(from, machO))] =
                saver.save(objectName(to, machO));
          }
          tableBuilt = true;
        }
        const std::string name = memberName(child);
        config.Common.InputFilename = saver.save(name);  // for messages

        llvm::SmallString<0> bytes;
        llvm::raw_svector_ostream out(bytes);
        if (auto copyErr = llvm::objcopy::executeObjcopyOnBinary(config, bin, out)) {
          return copyErr;
        }

        // Keeps the member's name, mode and timestamps; only the bytes change
        auto member = llvm::NewArchiveMember::getOldMember(child, true);
        if (!member) return member.takeError();
        member->Buf = llvm::MemoryBuffer::getMemBufferCopy(bytes, name);
        member->MemberName = member->Buf->getBufferIdentifier();
        members.push_back(std::move(*member));
        return llvm::Error::success();
      });
  if (err) return std::move(err);

  auto written = llvm::writeArchiveToBuffer(
      members, llvm::SymtabWritingMode::NormalSymtab, (*archive)->kind(),
      /*Deterministic=*/true, /*Thin=*/false);
  if (!written) return written.takeError();
  return (*written)->getBuffer().str();
}

std::map<std::string, std::string> listArchiveDefinitions(
    llvm::MemoryBufferRef archiveBytes) {
  std::map<std::string, std::string> definitions;
  auto archive = Archive::create(archiveBytes);
  if (!archive) {
    llvm::consumeError(archive.takeError());
    return definitions;
  }
  // Mach-O archives are told apart by their format, so no member is opened
  const auto kind = (*archive)->kind();
  const bool machO = kind == Archive::K_DARWIN || kind == Archive::K_DARWIN64;
  for (const Archive::Symbol& symbol : (*archive)->symbols()) {
    const std::string recorded = bareName(symbol.getName(), machO);
    definitions.emplace(stripBundlePrefix(recorded), recorded);
  }
  return definitions;
}

}  // namespace sun
