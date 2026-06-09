// moon.cpp — .moon bundle format reader/writer with protobuf metadata

#include "moon/moon.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

#include "struct_names.h"

namespace sun {

namespace {

/// Compute FNV-1a hash of data, return as 8-char hex string
std::string computeContentHash(const std::string& data) {
  constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
  constexpr uint64_t FNV_PRIME = 1099511628211ULL;

  uint64_t hash = FNV_OFFSET;
  for (unsigned char c : data) {
    hash ^= c;
    hash *= FNV_PRIME;
  }

  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(8) << (hash & 0xFFFFFFFF);
  return oss.str();
}

// Known external C runtime functions that should NOT be prefixed
bool shouldSkipRename(const std::string& name) {
  static const std::set<std::string> cRuntimeFunctions = {
      "malloc",    "free",     "realloc", "calloc",  "memset",  "memcpy",
      "memmove",   "memcmp",   "strlen",  "strcpy",  "strncpy", "strcmp",
      "strncmp",   "strcat",   "strncat", "printf",  "fprintf", "sprintf",
      "snprintf",  "puts",     "putchar", "getchar", "fopen",   "fclose",
      "fread",     "fwrite",   "fseek",   "ftell",   "fflush",  "exit",
      "abort",     "atexit",   "atoi",    "atof",    "atol",    "strtol",
      "strtod",    "qsort",    "bsearch", "rand",    "srand",   "time",
      "clock",     "difftime", "mktime",  "asctime", "ctime",   "gmtime",
      "localtime", "strftime", "sin",     "cos",     "tan",     "asin",
      "acos",      "atan",     "atan2",   "sinh",    "cosh",    "tanh",
      "exp",       "log",      "log10",   "pow",     "sqrt",    "ceil",
      "floor",     "fabs",     "fmod",    "frexp",   "ldexp",   "modf"};

  if (name.starts_with("llvm.")) return true;
  if (name.starts_with("$")) return true;
  if (cRuntimeFunctions.count(name)) return true;
  if (!name.empty() && name[0] == '_') return true;
  return false;
}

// Apply content hash prefix to all symbols in an LLVM module for isolation
void applySymbolPrefix(llvm::Module& module, const std::string& prefix) {
  for (auto& func : module.functions()) {
    if (!func.hasName() || func.getName().empty()) continue;
    if (func.isIntrinsic()) continue;
    std::string originalName = func.getName().str();
    if (!shouldSkipRename(originalName)) {
      func.setName(prefix + "_" + originalName);
    }
  }

  for (auto& global : module.globals()) {
    if (!global.hasName() || global.getName().empty()) continue;
    std::string originalName = global.getName().str();
    if (!shouldSkipRename(originalName)) {
      global.setName(prefix + "_" + originalName);
    }
  }

  for (auto* structTy : module.getIdentifiedStructTypes()) {
    if (!structTy->hasName()) continue;
    llvm::StringRef name = structTy->getName();
    if (name.starts_with("llvm.")) continue;
    if (name.starts_with("$")) continue;
    bool isRuntimeType = false;
    for (const auto& info : sun::StructNames::All) {
      if (name.starts_with(info.name)) {
        isRuntimeType = true;
        break;
      }
    }
    if (isRuntimeType) continue;
    structTy->setName(prefix + "_" + name.str());
  }
}

// Apply symbol prefix to names in protobuf metadata
void applyMetadataPrefix(moon::ModuleMetadata& metadata,
                         const std::string& prefix) {
  // Prefix function names in FunctionDef protos
  for (int i = 0; i < metadata.functions_size(); ++i) {
    auto* func = metadata.mutable_functions(i);
    auto* proto = func->mutable_proto();
    std::string name = proto->name();
    if (!name.empty() && !name.starts_with("$")) {
      proto->set_name(prefix + "_" + name);
    }
  }

  // Prefix class names, implemented interfaces, and method names in ClassDef
  // protos
  for (int i = 0; i < metadata.classes_size(); ++i) {
    auto* cls = metadata.mutable_classes(i);
    std::string name = cls->name();
    if (!name.empty() && !name.starts_with("$")) {
      cls->set_name(prefix + "_" + name);
    }
    // Prefix implemented interface names
    for (int j = 0; j < cls->implemented_interfaces_size(); ++j) {
      auto* iface = cls->mutable_implemented_interfaces(j);
      std::string ifaceName = iface->name();
      if (!ifaceName.empty() && !ifaceName.starts_with("$")) {
        iface->set_name(prefix + "_" + ifaceName);
      }
    }
    // Also prefix method names
    for (int j = 0; j < cls->methods_size(); ++j) {
      auto* method = cls->mutable_methods(j);
      auto* funcProto = method->mutable_function()->mutable_proto();
      std::string methodName = funcProto->name();
      if (!methodName.empty() && !methodName.starts_with("$")) {
        funcProto->set_name(prefix + "_" + methodName);
      }
    }
  }

  // Prefix interface names and method names in InterfaceDef protos
  for (int i = 0; i < metadata.interfaces_size(); ++i) {
    auto* iface = metadata.mutable_interfaces(i);
    std::string name = iface->name();
    if (!name.empty() && !name.starts_with("$")) {
      iface->set_name(prefix + "_" + name);
    }
    for (int j = 0; j < iface->methods_size(); ++j) {
      auto* method = iface->mutable_methods(j);
      auto* funcProto = method->mutable_function()->mutable_proto();
      std::string methodName = funcProto->name();
      if (!methodName.empty() && !methodName.starts_with("$")) {
        funcProto->set_name(prefix + "_" + methodName);
      }
    }
  }

  // Prefix enum names
  for (int i = 0; i < metadata.enums_size(); ++i) {
    auto* enumDef = metadata.mutable_enums(i);
    std::string name = enumDef->name();
    if (!name.empty() && !name.starts_with("$")) {
      enumDef->set_name(prefix + "_" + name);
    }
  }
}

}  // namespace

//===----------------------------------------------------------------------===//
// SunLibWriter
//===----------------------------------------------------------------------===//

SunLibWriter::SunLibWriter() = default;

void SunLibWriter::addModule(llvm::Module& module,
                             const moon::ModuleMetadata& metadata) {
  ModuleData data;

  // Serialize LLVM bitcode
  llvm::raw_string_ostream bitcodeStream(data.bitcode);
  llvm::WriteBitcodeToFile(module, bitcodeStream);
  bitcodeStream.flush();

  // Store metadata (hash will be computed in write() from combined content)
  data.metadata = metadata;

  modules_.push_back(std::move(data));
}

bool SunLibWriter::write(const std::filesystem::path& outputPath) {
  std::ofstream out(outputPath, std::ios::binary);
  if (!out) {
    error_ = "Failed to open output file: " + outputPath.string();
    return false;
  }

  // Compute a single content hash from ALL original bitcodes in the bundle
  std::string combinedContent;
  for (const auto& mod : modules_) {
    combinedContent += mod.bitcode;
  }
  std::string bundleHash = computeContentHash(combinedContent);
  std::string prefix = "$" + bundleHash + "$";

  // Write header (will update indexOffset later)
  SunLibHeader header;
  header.moduleCount = static_cast<uint32_t>(modules_.size());
  auto headerPos = out.tellp();
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));

  // Write module data and build index
  std::vector<ModuleIndexEntry> index;
  llvm::LLVMContext tempContext;

  for (auto& mod : modules_) {
    ModuleIndexEntry entry;
    entry.moduleKey = mod.metadata.source_hash();

    // Re-parse the bitcode to apply symbol prefixes
    auto memBuffer = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(mod.bitcode.data(), mod.bitcode.size()),
        mod.metadata.source_hash(), false);

    auto moduleOrErr =
        llvm::parseBitcodeFile(memBuffer->getMemBufferRef(), tempContext);
    if (!moduleOrErr) {
      llvm::consumeError(moduleOrErr.takeError());
      error_ = "Failed to re-parse bitcode for symbol prefixing";
      return false;
    }

    auto& parsedModule = *moduleOrErr;

    // Apply symbol prefixes to LLVM module
    applySymbolPrefix(*parsedModule, prefix);

    // Re-serialize the prefixed module
    std::string prefixedBitcode;
    llvm::raw_string_ostream bitcodeStream(prefixedBitcode);
    llvm::WriteBitcodeToFile(*parsedModule, bitcodeStream);
    bitcodeStream.flush();

    // Write prefixed bitcode
    entry.bitcodeOffset = static_cast<uint64_t>(out.tellp());
    entry.bitcodeSize = prefixedBitcode.size();
    out.write(prefixedBitcode.data(),
              static_cast<std::streamsize>(prefixedBitcode.size()));

    // NOTE: Do NOT prefix metadata names. The semantic analyzer will compute
    // fully-qualified names based on the import scope path (content hash) and
    // the module namespace wrapping. The metadata stores base names only.

    // Set shared bundle hash
    mod.metadata.set_content_hash(bundleHash);

    // Serialize metadata as protobuf
    std::string metadataStr;
    if (!mod.metadata.SerializeToString(&metadataStr)) {
      error_ = "Failed to serialize metadata protobuf";
      return false;
    }

    // Write metadata
    entry.metadataOffset = static_cast<uint64_t>(out.tellp());
    entry.metadataSize = metadataStr.size();
    out.write(metadataStr.data(),
              static_cast<std::streamsize>(metadataStr.size()));

    index.push_back(entry);
  }

  // Write index
  header.indexOffset = static_cast<uint32_t>(out.tellp());

  uint32_t indexCount = static_cast<uint32_t>(index.size());
  out.write(reinterpret_cast<const char*>(&indexCount), sizeof(indexCount));

  for (const auto& entry : index) {
    uint32_t keyLen = static_cast<uint32_t>(entry.moduleKey.size());
    out.write(reinterpret_cast<const char*>(&keyLen), sizeof(keyLen));
    out.write(entry.moduleKey.data(), keyLen);
    out.write(reinterpret_cast<const char*>(&entry.bitcodeOffset),
              sizeof(entry.bitcodeOffset));
    out.write(reinterpret_cast<const char*>(&entry.bitcodeSize),
              sizeof(entry.bitcodeSize));
    out.write(reinterpret_cast<const char*>(&entry.metadataOffset),
              sizeof(entry.metadataOffset));
    out.write(reinterpret_cast<const char*>(&entry.metadataSize),
              sizeof(entry.metadataSize));
  }

  // Update header with index offset
  out.seekp(headerPos);
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));

  if (!out.good()) {
    error_ = "Failed to write bundle file";
    return false;
  }

  return true;
}

//===----------------------------------------------------------------------===//
// SunLibReader
//===----------------------------------------------------------------------===//

std::unique_ptr<SunLibReader> SunLibReader::open(
    const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return nullptr;
  }

  // Read header
  SunLibHeader header;
  in.read(reinterpret_cast<char*>(&header), sizeof(header));

  if (header.magic != SunLibHeader::MAGIC) {
    return nullptr;
  }
  // Accept V2 format (protobuf metadata)
  if (header.version != 2) {
    return nullptr;
  }

  auto reader = std::unique_ptr<SunLibReader>(new SunLibReader());
  reader->path_ = path;

  // Read index
  in.seekg(header.indexOffset);

  uint32_t indexCount;
  in.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));

  for (uint32_t i = 0; i < indexCount; ++i) {
    ModuleIndexEntry entry;

    uint32_t keyLen;
    in.read(reinterpret_cast<char*>(&keyLen), sizeof(keyLen));

    entry.moduleKey.resize(keyLen);
    in.read(entry.moduleKey.data(), keyLen);

    in.read(reinterpret_cast<char*>(&entry.bitcodeOffset),
            sizeof(entry.bitcodeOffset));
    in.read(reinterpret_cast<char*>(&entry.bitcodeSize),
            sizeof(entry.bitcodeSize));
    in.read(reinterpret_cast<char*>(&entry.metadataOffset),
            sizeof(entry.metadataOffset));
    in.read(reinterpret_cast<char*>(&entry.metadataSize),
            sizeof(entry.metadataSize));

    reader->indexMap_[entry.moduleKey] = reader->index_.size();
    reader->index_.push_back(entry);
  }

  return reader;
}

bool SunLibReader::hasModule(const std::string& moduleKey) const {
  return indexMap_.find(moduleKey) != indexMap_.end();
}

std::vector<std::string> SunLibReader::listModules() const {
  std::vector<std::string> result;
  result.reserve(index_.size());
  for (const auto& entry : index_) {
    result.push_back(entry.moduleKey);
  }
  return result;
}

bool SunLibReader::readBytes(uint64_t offset, uint64_t size,
                             std::vector<char>& buffer) {
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    error_ = "Failed to open bundle file";
    return false;
  }

  in.seekg(static_cast<std::streamoff>(offset));
  buffer.resize(size);
  in.read(buffer.data(), static_cast<std::streamsize>(size));

  if (!in.good()) {
    error_ = "Failed to read from bundle file";
    return false;
  }

  return true;
}

const moon::ModuleMetadata* SunLibReader::getMetadata(
    const std::string& moduleKey) {
  // Check cache
  auto cacheIt = metadataCache_.find(moduleKey);
  if (cacheIt != metadataCache_.end()) {
    return &cacheIt->second;
  }

  // Find index entry
  auto it = indexMap_.find(moduleKey);
  if (it == indexMap_.end()) {
    error_ = "Module not found: " + moduleKey;
    return nullptr;
  }

  const auto& entry = index_[it->second];

  // Read metadata bytes
  std::vector<char> buffer;
  if (!readBytes(entry.metadataOffset, entry.metadataSize, buffer)) {
    return nullptr;
  }

  // Parse protobuf
  moon::ModuleMetadata metadata;
  if (!metadata.ParseFromArray(buffer.data(),
                               static_cast<int>(buffer.size()))) {
    error_ = "Failed to parse metadata protobuf for: " + moduleKey;
    return nullptr;
  }

  metadataCache_[moduleKey] = std::move(metadata);
  return &metadataCache_[moduleKey];
}

std::unique_ptr<llvm::Module> SunLibReader::loadModule(
    const std::string& moduleKey, llvm::LLVMContext& context) {
  auto it = indexMap_.find(moduleKey);
  if (it == indexMap_.end()) {
    error_ = "Module not found in index: " + moduleKey;
    return nullptr;
  }

  const auto& entry = index_[it->second];

  std::vector<char> buffer;
  if (!readBytes(entry.bitcodeOffset, entry.bitcodeSize, buffer)) {
    error_ = "Failed to read bitcode at offset " +
             std::to_string(entry.bitcodeOffset);
    return nullptr;
  }

  auto memBuffer = llvm::MemoryBuffer::getMemBuffer(
      llvm::StringRef(buffer.data(), buffer.size()), moduleKey, false);

  auto moduleOrErr =
      llvm::parseBitcodeFile(memBuffer->getMemBufferRef(), context);
  if (!moduleOrErr) {
    std::string errStr;
    llvm::raw_string_ostream errOS(errStr);
    errOS << moduleOrErr.takeError();
    error_ = "Failed to parse bitcode for: " + moduleKey + " - " + errStr;
    return nullptr;
  }

  return std::move(*moduleOrErr);
}

}  // namespace sun
