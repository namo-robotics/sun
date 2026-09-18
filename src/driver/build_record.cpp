// build_record.cpp — see build_record.h

#include "driver/build_record.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <filesystem>
#include <sstream>

#include "moon_bundling/moon.h"

namespace sun {

namespace {

constexpr const char* kRecordHeader = "sun-build-record 1";
// Mach-O names a section by segment and section; the reader sees only the
// section part, which is why both spellings end the same way.
constexpr const char* kElfSection = ".sun_build";
constexpr const char* kMachOSection = "__TEXT,__sun_build";
constexpr const char* kMachOSectionName = "__sun_build";

std::string renderRecord(const BuildRecord& record) {
  std::string text = kRecordHeader;
  text += "\nhash=" + record.inputHash;
  text += std::string("\ntests=") + (record.hasTests ? "1" : "0");
  text += std::string("\nexecutable=") + (record.hasExecutable ? "1" : "0");
  text += "\n";
  return text;
}

std::optional<BuildRecord> parseRecord(const std::string& text) {
  std::istringstream lines(text);
  std::string line;
  if (!std::getline(lines, line) || line != kRecordHeader) return std::nullopt;
  BuildRecord record;
  while (std::getline(lines, line)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (key == "hash") record.inputHash = value;
    if (key == "tests") record.hasTests = value == "1";
    if (key == "executable") record.hasExecutable = value == "1";
  }
  if (record.inputHash.empty()) return std::nullopt;
  return record;
}

}  // namespace

void embedBuildRecord(llvm::Module& module, const BuildRecord& record) {
  auto* data = llvm::ConstantDataArray::getString(
      module.getContext(), renderRecord(record), /*AddNull=*/false);
  auto* global = new llvm::GlobalVariable(module, data->getType(),
                                          /*isConstant=*/true,
                                          llvm::GlobalValue::InternalLinkage,
                                          data, "sun.build_record");
  const llvm::Triple triple(module.getTargetTriple());
  global->setSection(triple.isOSBinFormatMachO() ? kMachOSection : kElfSection);
  global->setAlignment(llvm::Align(1));
  // Nothing refers to the record, so without this the optimizer and the
  // linker would both be right to drop it.
  llvm::appendToUsed(module, {global});
}

std::optional<BuildRecord> readBuildRecord(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
  auto binary = llvm::object::ObjectFile::createObjectFile(path);
  if (!binary) {
    llvm::consumeError(binary.takeError());
    return std::nullopt;
  }
  for (const auto& section : binary->getBinary()->sections()) {
    auto name = section.getName();
    if (!name) {
      llvm::consumeError(name.takeError());
      continue;
    }
    if (*name != kElfSection && *name != kMachOSectionName) continue;
    auto contents = section.getContents();
    if (!contents) {
      llvm::consumeError(contents.takeError());
      return std::nullopt;
    }
    return parseRecord(contents->str());
  }
  return std::nullopt;
}

std::optional<std::string> readMoonInputHash(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
  auto reader = MoonReader::open(path);
  if (!reader) return std::nullopt;
  const auto modules = reader->listModules();
  const auto* first =
      modules.empty() ? nullptr : reader->getMetadata(modules[0]);
  if (!first || first->content_hash().empty()) return std::nullopt;
  return first->content_hash();
}

}  // namespace sun
