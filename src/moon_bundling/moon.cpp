// moon.cpp — .moon bundle format reader/writer with protobuf metadata

#include "moon_bundling/moon.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <iomanip>
#include <sstream>

namespace sun {

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

//===----------------------------------------------------------------------===//
// MoonWriter
//===----------------------------------------------------------------------===//

MoonWriter::MoonWriter(std::string bundleHash)
    : bundleHash_(std::move(bundleHash)) {}

void MoonWriter::addModule(llvm::Module& module,
                           const moon::ModuleMetadata& metadata) {
  ModuleData data;

  // The same module is normally handed in once per exported module; serialize
  // it once and let the later entries reference the same blob.
  auto cached = blobIndexByModule_.find(&module);
  if (cached != blobIndexByModule_.end()) {
    data.blobIndex = cached->second;
  } else {
    std::string bitcode;
    llvm::raw_string_ostream bitcodeStream(bitcode);
    llvm::WriteBitcodeToFile(module, bitcodeStream);
    bitcodeStream.flush();

    data.blobIndex = blobs_.size();
    blobs_.push_back(std::move(bitcode));
    blobIndexByModule_[&module] = data.blobIndex;
  }

  data.metadata = metadata;

  // Stamp the target the bitcode was compiled for; the linker refuses to mix
  // targets, since struct layouts and ABI decisions are baked into bitcode.
  data.metadata.set_target_triple(module.getTargetTriple());

  modules_.push_back(std::move(data));
}

void MoonWriter::addNativeArchive(std::string name, std::string data) {
  nativeArchives_.emplace_back(std::move(name), std::move(data));
}

bool MoonWriter::write(const std::filesystem::path& outputPath) {
  std::ofstream out(outputPath, std::ios::binary);
  if (!out) {
    error_ = "Failed to open output file: " + outputPath.string();
    return false;
  }

  // Write header (will update indexOffset later)
  MoonHeader header;
  auto headerPos = out.tellp();
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));

  // Write module data and build index
  std::vector<ModuleIndexEntry> index;

  // Each distinct blob is written once; every module that shares it points
  // at the same region. A bundle built from one compilation unit therefore
  // stores its code once instead of once per exported module.
  struct BlobLocation {
    uint64_t offset = 0;
    uint64_t size = 0;
    bool written = false;
  };
  std::vector<BlobLocation> blobLocations(blobs_.size());

  for (auto& mod : modules_) {
    ModuleIndexEntry entry;
    entry.moduleKey = mod.metadata.source_hash();

    auto& location = blobLocations[mod.blobIndex];
    if (!location.written) {
      // The bitcode is stored as compiled: the compiler already spelled the
      // bundle's own symbols with the `$hash$_` prefix importers look for.
      const std::string& bitcode = blobs_[mod.blobIndex];
      location.offset = static_cast<uint64_t>(out.tellp());
      location.size = bitcode.size();
      location.written = true;
      out.write(bitcode.data(), static_cast<std::streamsize>(bitcode.size()));
    }

    entry.bitcodeOffset = location.offset;
    entry.bitcodeSize = location.size;

    // Metadata keeps base names only: importers derive the fully-qualified
    // names from this hash and the module path, exactly as the bundle's own
    // compilation did.
    mod.metadata.set_content_hash(bundleHash_);

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

  // Write carried native archives, recording where each landed
  std::vector<NativeArchiveEntry> archiveIndex;
  archiveIndex.reserve(nativeArchives_.size());
  for (const auto& [name, data] : nativeArchives_) {
    NativeArchiveEntry entry;
    entry.name = name;
    entry.offset = static_cast<uint64_t>(out.tellp());
    entry.size = data.size();
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    archiveIndex.push_back(std::move(entry));
  }

  // Write index
  header.indexOffset = static_cast<uint64_t>(out.tellp());

  uint64_t indexCount = index.size();
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

  // Native archive index follows the module index
  uint64_t archiveCount = archiveIndex.size();
  out.write(reinterpret_cast<const char*>(&archiveCount), sizeof(archiveCount));
  for (const auto& entry : archiveIndex) {
    uint32_t nameLen = static_cast<uint32_t>(entry.name.size());
    out.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
    out.write(entry.name.data(), nameLen);
    out.write(reinterpret_cast<const char*>(&entry.offset),
              sizeof(entry.offset));
    out.write(reinterpret_cast<const char*>(&entry.size), sizeof(entry.size));
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
// MoonReader
//===----------------------------------------------------------------------===//

std::unique_ptr<MoonReader> MoonReader::open(
    const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return nullptr;
  }

  // Read header
  MoonHeader header;
  in.read(reinterpret_cast<char*>(&header), sizeof(header));

  if (header.magic != MoonHeader::MAGIC) {
    return nullptr;
  }
  // Reject bundles built for a different ABI/format version
  if (header.version != MoonHeader::VERSION) {
    return nullptr;
  }

  auto reader = std::unique_ptr<MoonReader>(new MoonReader());
  reader->path_ = path;

  // Read index
  in.seekg(static_cast<std::streamoff>(header.indexOffset));

  uint64_t indexCount = 0;
  in.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));

  for (uint64_t i = 0; i < indexCount; ++i) {
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

  // Native archive index follows the module index
  uint64_t archiveCount = 0;
  in.read(reinterpret_cast<char*>(&archiveCount), sizeof(archiveCount));
  if (in.good()) {
    for (uint64_t i = 0; i < archiveCount; ++i) {
      NativeArchiveEntry entry;
      uint32_t nameLen = 0;
      in.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
      entry.name.resize(nameLen);
      in.read(entry.name.data(), nameLen);
      in.read(reinterpret_cast<char*>(&entry.offset), sizeof(entry.offset));
      in.read(reinterpret_cast<char*>(&entry.size), sizeof(entry.size));
      if (!in.good()) break;
      reader->nativeArchives_.push_back(std::move(entry));
    }
  }

  return reader;
}

bool MoonReader::readNativeArchive(const std::string& name,
                                   std::vector<char>& out) {
  for (const auto& entry : nativeArchives_) {
    if (entry.name == name) {
      return readBytes(entry.offset, entry.size, out);
    }
  }
  error_ = "Native archive not found in bundle: " + name;
  return false;
}

bool MoonReader::hasModule(const std::string& moduleKey) const {
  return indexMap_.find(moduleKey) != indexMap_.end();
}

std::vector<std::string> MoonReader::listModules() const {
  std::vector<std::string> result;
  result.reserve(index_.size());
  for (const auto& entry : index_) {
    result.push_back(entry.moduleKey);
  }
  return result;
}

bool MoonReader::readBytes(uint64_t offset, uint64_t size,
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

const moon::ModuleMetadata* MoonReader::getMetadata(
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

std::string MoonReader::getBitcodeId(const std::string& moduleKey) const {
  auto it = indexMap_.find(moduleKey);
  if (it == indexMap_.end()) return "";
  const auto& entry = index_[it->second];
  return path_.string() + ":" + std::to_string(entry.bitcodeOffset) + ":" +
         std::to_string(entry.bitcodeSize);
}

std::string MoonReader::getTargetTriple() {
  if (index_.empty()) return "";
  const auto* metadata = getMetadata(index_.front().moduleKey);
  return metadata ? metadata->target_triple() : "";
}

std::unique_ptr<llvm::Module> MoonReader::loadModule(
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

  auto memBuffer = llvm::MemoryBuffer::getMemBufferCopy(
      llvm::StringRef(buffer.data(), buffer.size()), moduleKey);

  // Lazy: prototypes and globals are parsed now, function bodies only on
  // materialization (i.e. when the module actually gets linked). Callers
  // that just scan declarations never pay for body parsing.
  auto moduleOrErr =
      llvm::getOwningLazyBitcodeModule(std::move(memBuffer), context);
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
