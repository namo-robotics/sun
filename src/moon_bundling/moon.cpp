// moon.cpp — .moon bundle format reader/writer with protobuf metadata

#include "moon_bundling/moon.h"

#include <google/protobuf/util/json_util.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "abi/library_abi_snapshot.h"
#include "abi/native_requirements.h"
#include "abi/nominal_identity.h"
#include "abi/semantic_requirements.h"
#include "abi/support_inventory.h"
#include "support/error.h"
#include "support/terminal.h"

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {
/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/** Reject incomplete or malformed identities at bundle boundaries. */
void validateBundleHash(const std::string& hash) {
  if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) {
    sun::support::logAndThrowError(
        "Bundle identity must be a full lowercase SHA-256 digest");
  }
}

}  // namespace

/** Returns the hexadecimal SHA-256 digest of the supplied contents. */
std::string computeSha256Hex(llvm::StringRef data) {
  llvm::SHA256 sha;
  sha.update(data);
  std::string result;
  for (uint8_t byte : sha.final()) {
    constexpr char hex[] = "0123456789abcdef";
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

//===----------------------------------------------------------------------===//
// MoonWriter
//===----------------------------------------------------------------------===//

MoonWriter::MoonWriter(std::string bundleHash)
    : bundleHash_(std::move(bundleHash)) {
  validateBundleHash(bundleHash_);
}

void MoonWriter::addModule(llvm::Module& module,
                           const moon::ModuleMetadata& metadata) {
  ModuleData data;

  // The same module is normally handed in once per exported module; serialize
  // it once and let the later entries reference the same blob.
  auto cached = blobIndexByModule_.find(&module);
  if (cached != blobIndexByModule_.end()) {
    data.blobIndex = cached->second;
  } else {
    auto generated = abi::describeContract(module);
    if (contract_.encoding_schema() == 0) {
      contract_ = std::move(generated);
    } else {
      abi::mergeContractRequirements(contract_, generated);
    }
    std::string bitcode;
    llvm::raw_string_ostream bitcodeStream(bitcode);
    llvm::WriteBitcodeToFile(module, bitcodeStream);
    bitcodeStream.flush();

    data.blobIndex = blobs_.size();
    blobs_.push_back(std::move(bitcode));
    blobIndexByModule_[&module] = data.blobIndex;
  }

  data.metadata = metadata;

  // Only advertise shapes whose ordinary method implementations are present.
  std::set<std::string> definitions;
  for (const auto& function : module) {
    if (!function.isDeclaration()) {
      definitions.insert(function.getName().str());
    }
  }
  for (auto& cls : *data.metadata.mutable_classes()) {
    auto* candidates = cls.mutable_compiled_specializations();
    for (int i = candidates->size() - 1; i >= 0; --i) {
      const auto& candidate = candidates->Get(i);
      bool complete = true;
      for (const auto& symbol : candidate.method_symbols()) {
        if (!definitions.count(symbol)) {
          complete = false;
        }
      }
      if (!complete) {
        candidates->DeleteSubrange(i, 1);
      }
    }
  }

  // Stamp the target the bitcode was compiled for; the linker refuses to mix
  // targets, since struct layouts and ABI decisions are baked into bitcode.
  data.metadata.set_target_triple(module.getTargetTriple());

  modules_.push_back(std::move(data));
}

void MoonWriter::setLibraryId(const std::string& libraryId) {
  if (contract_.encoding_schema() == 0) {
    sun::support::logAndThrowError("Add emitted code before library identity");
  }
  abi::validateLibraryId(libraryId);
  contract_.set_library_id(libraryId);
}

void MoonWriter::inheritContract(const abi::BundleContract& contract) {
  if (contract_.encoding_schema() == 0) {
    sun::support::logAndThrowError(
        "Add emitted code before inherited contracts");
  }
  abi::mergeContractRequirements(contract_, contract);
}

void MoonWriter::declareNativeRequirements(
    const std::vector<std::string>& cpuFeatures,
    const std::vector<std::string>& runtimeServices) {
  abi::addNativeRequirements(contract_, cpuFeatures, runtimeServices);
}

void MoonWriter::addNativeArchive(std::string archiveSetHash, std::string name,
                                  std::string data) {
  nativeArchives_.push_back(
      {std::move(archiveSetHash), std::move(name), std::move(data)});
}

bool MoonWriter::write(const std::filesystem::path& outputPath) {
  if (modules_.empty()) {
    error_ = "Moon bundle requires a metadata-bearing module";
    return false;
  }
  if (!nativeArchives_.empty()) {
    std::vector<abi::NativeArchiveInput> archives;
    for (const auto& archive : nativeArchives_) {
      archives.push_back({llvm::MemoryBufferRef(archive.data, archive.name),
                          "$" + archive.archiveSetHash + "$_"});
    }
    std::set<std::string> runtimeServices;
    std::set<std::string> linkSymbols(contract_.native_link_symbols().begin(),
                                      contract_.native_link_symbols().end());
    abi::validateNativeArchives(archives, contract_.target(), &runtimeServices,
                                &linkSymbols);
    contract_.clear_native_link_symbols();
    for (const auto& symbol : linkSymbols) {
      contract_.add_native_link_symbols(symbol);
    }
    for (const auto& service : runtimeServices) {
      const auto name = "native.runtime." + service;
      bool present = false;
      for (const auto& rule : contract_.requirements()) {
        if (rule.name() == name) {
          present = true;
        }
      }
      if (!present) {
        auto* rule = contract_.add_requirements();
        rule->set_name(name);
        rule->set_implementation(abi::emissionImplementation(rule->name()));
      }
    }
    const auto nativeRuleName = abi::nativeArchiveRule(contract_.target());
    bool nativeRule = false;
    for (const auto& rule : contract_.requirements()) {
      if (rule.name() == nativeRuleName) {
        nativeRule = true;
      }
    }
    if (!nativeRule) {
      auto* rule = contract_.add_requirements();
      rule->set_name(nativeRuleName);
      rule->set_implementation(abi::emissionImplementation(rule->name()));
    }
  }
  bool metadataRule = false;
  for (const auto& rule : contract_.requirements()) {
    if (rule.name() == "moon.metadata") {
      metadataRule = true;
    }
  }
  if (!metadataRule) {
    auto* rule = contract_.add_requirements();
    rule->set_name("moon.metadata");
    rule->set_implementation(abi::emissionImplementation("moon.metadata"));
  }
  std::set<std::string> semantics(contract_.semantic_records().begin(),
                                  contract_.semantic_records().end());
  for (const auto& module : modules_) {
    semantics.insert(abi::describeSemantics(module.metadata));
  }
  contract_.clear_semantic_records();
  for (const auto& record : semantics) {
    contract_.add_semantic_records(record);
  }
  if (contract_.has_library_id()) {
    std::vector<const moon::ModuleMetadata*> metadata;
    for (const auto& module : modules_) {
      metadata.push_back(&module.metadata);
    }
    *contract_.mutable_library_abi() =
        abi::describeLibraryAbiSnapshot(contract_, bundleHash_, metadata);
  }
  contract_.set_compatibility_hash(abi::hashBundleContract(contract_));
  moon::MoonLibrary bundleMetadata;
  *bundleMetadata.mutable_bundle_contract() = contract_;
  bundleMetadata.set_content_hash(bundleHash_);
  std::ofstream out(outputPath, std::ios::binary);
  if (!out) {
    error_ = "Failed to open output file: " + outputPath.string();
    return false;
  }

  // Write the header, then fill in the library location after writing payloads.
  MoonHeader header;
  auto headerPos = out.tellp();
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));

  /**
   * Each distinct blob is written once; every module that shares it points
   * at the same region. A bundle built from one compilation unit therefore
   * stores its code once instead of once per exported module.
   */
  struct BlobLocation {
    uint64_t offset = 0;
    uint64_t size = 0;
    bool written = false;
  };
  std::vector<BlobLocation> blobLocations(blobs_.size());

  for (auto& mod : modules_) {
    auto* entry = bundleMetadata.add_modules();

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

    entry->set_bitcode_offset(location.offset);
    entry->set_bitcode_size(location.size);

    // Declarations belong to this bundle; their type references already carry
    // the exact identities bound in the analyzed program.
    mod.metadata.set_content_hash(bundleHash_);

    *entry->mutable_metadata() = mod.metadata;
  }

  for (const auto& archive : nativeArchives_) {
    auto* entry = bundleMetadata.add_native_archives();
    entry->set_archive_set_hash(archive.archiveSetHash);
    entry->set_name(archive.name);
    entry->set_offset(static_cast<uint64_t>(out.tellp()));
    entry->set_size(archive.data.size());
    out.write(archive.data.data(),
              static_cast<std::streamsize>(archive.data.size()));
  }
  const auto bundleBytes = bundleMetadata.SerializeAsString();
  header.bundleMetadataOffset = static_cast<uint64_t>(out.tellp());
  header.bundleMetadataSize = bundleBytes.size();
  out.write(bundleBytes.data(),
            static_cast<std::streamsize>(bundleBytes.size()));

  // Locate the complete library record from the header.
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
  auto file = llvm::MemoryBuffer::getFile(path.string(), false, false);
  if (!file) {
    return nullptr;
  }
  constexpr uint64_t maxBundleBytes = uint64_t{2} * 1024 * 1024 * 1024;
  if ((*file)->getBufferSize() > maxBundleBytes) {
    sun::support::logAndThrowError(
        "Moon bundle exceeds reader resource limit: " + path.string());
  }
  auto reader = std::unique_ptr<MoonReader>(new MoonReader());
  reader->path_ = path;
  reader->bytes_ =
      std::make_shared<const std::string>((*file)->getBuffer().str());
  const uint64_t size = reader->bytes_->size();
  uint64_t cursor = 0;
  /** Stops parsing malformed bytes before allocation or semantic use. */
  auto malformed = [&]() {
    sun::support::logAndThrowError("Malformed Moon container: " +
                                   path.string());
  };
  /** Reads the legacy host-order envelope without unaligned memory access. */
  auto read = [&](auto& value) {
    if (cursor > size || sizeof(value) > size - cursor) {
      malformed();
    }
    std::memcpy(&value, reader->bytes_->data() + cursor, sizeof(value));
    cursor += sizeof(value);
  };
  MoonHeader header;
  read(header.magic);
  read(header.version);
  if (header.magic != MoonHeader::MAGIC) {
    malformed();
  }
  if (header.version != MoonHeader::VERSION) {
    sun::support::logAndThrowError(
        "Unsupported moon bundle format version " +
        std::to_string(header.version) + " in '" + path.string() +
        "': compatibility cannot be established; rebuild the bundle");
  }
  read(header.bundleMetadataOffset);
  read(header.bundleMetadataSize);
  if (header.bundleMetadataOffset < sizeof(MoonHeader) ||
      header.bundleMetadataOffset > size || !header.bundleMetadataSize ||
      header.bundleMetadataSize > 64 * 1024 * 1024 ||
      header.bundleMetadataSize != size - header.bundleMetadataOffset) {
    malformed();
  }
  if (!reader->bundleMetadata_.ParseFromArray(
          reader->bytes_->data() + header.bundleMetadataOffset,
          static_cast<int>(header.bundleMetadataSize))) {
    malformed();
  }
  const auto& library = reader->bundleMetadata_;
  validateBundleHash(library.content_hash());
  if (!library.has_bundle_contract()) {
    sun::support::logAndThrowError(
        "Dependency '" + path.string() +
        "': compatibility cannot be established; missing bundle contract; "
        "rebuild the bundle");
  }
  if (library.modules_size() == 0 || library.modules_size() > 1000000 ||
      library.native_archives_size() > 1000000) {
    malformed();
  }
  /** Records regions so unrelated overlapping records cannot be accepted. */
  struct Region {
    uint64_t start;
    uint64_t size;
    bool bitcode;
  };
  std::vector<Region> regions;
  /** Validates offsets with subtraction to avoid integer overflow. */
  auto region = [&](uint64_t offset, uint64_t length, bool bitcode) {
    if (offset < sizeof(MoonHeader) || offset > header.bundleMetadataOffset ||
        length > header.bundleMetadataOffset - offset) {
      malformed();
    }
    regions.push_back({offset, length, bitcode});
  };
  for (const auto& module : library.modules()) {
    if (!module.has_metadata()) {
      malformed();
    }
    const auto& metadata = module.metadata();
    if (metadata.source_hash().empty() ||
        metadata.source_hash().size() > 1024 * 1024 ||
        metadata.content_hash() != library.content_hash()) {
      malformed();
    }
    ModuleIndexEntry entry{metadata.source_hash(), module.bitcode_offset(),
                           module.bitcode_size()};
    region(entry.bitcodeOffset, entry.bitcodeSize, true);
    if (!reader->indexMap_.emplace(entry.moduleKey, reader->index_.size())
             .second) {
      malformed();
    }
    reader->index_.push_back(std::move(entry));
  }
  std::set<std::pair<std::string, std::string>> archives;
  for (const auto& archive : library.native_archives()) {
    NativeArchiveEntry entry{archive.archive_set_hash(), archive.name(),
                             archive.offset(), archive.size()};
    if (entry.name.empty() || entry.name.size() > 1024 * 1024 ||
        std::filesystem::path(entry.name).filename() != entry.name ||
        entry.name == "." || entry.name == ".." ||
        entry.archiveSetHash.empty() ||
        entry.archiveSetHash.size() > 1024 * 1024 ||
        entry.archiveSetHash.find_first_not_of("0123456789abcdef") !=
            std::string::npos ||
        !archives.emplace(entry.archiveSetHash, entry.name).second) {
      malformed();
    }
    region(entry.offset, entry.size, false);
    reader->nativeArchives_.push_back(std::move(entry));
  }
  std::sort(regions.begin(), regions.end(),
            [](const Region& a, const Region& b) {
              return std::tie(a.start, a.size) < std::tie(b.start, b.size);
            });
  for (size_t i = 1; i < regions.size(); ++i) {
    const auto& a = regions[i - 1];
    const auto& b = regions[i];
    if (b.start < a.start + a.size &&
        !(a.bitcode && b.bitcode && a.start == b.start && a.size == b.size)) {
      malformed();
    }
  }
  return reader;
}

bool MoonReader::writeDebugJson(const std::filesystem::path& outputPath) {
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  options.always_print_primitive_fields = true;
  llvm::json::Array modules;
  for (const auto& entry : index_) {
    const auto* metadata = getMetadata(entry.moduleKey);
    if (!metadata) {
      return false;
    }
    std::string json;
    auto status =
        google::protobuf::util::MessageToJsonString(*metadata, &json, options);
    if (!status.ok()) {
      error_ = "Failed to convert moon metadata to JSON: " + status.ToString();
      return false;
    }
    auto value = llvm::json::parse(json);
    if (!value) {
      error_ =
          "Failed to parse metadata JSON: " + llvm::toString(value.takeError());
      return false;
    }
    modules.push_back(
        llvm::json::Object{{"module_key", entry.moduleKey},
                           {"bitcode_offset", entry.bitcodeOffset},
                           {"bitcode_size", entry.bitcodeSize},
                           {"metadata", std::move(*value)}});
  }
  llvm::json::Array archives;
  for (const auto& entry : nativeArchives_) {
    archives.push_back(
        llvm::json::Object{{"archive_set_hash", entry.archiveSetHash},
                           {"name", entry.name},
                           {"offset", entry.offset},
                           {"size", entry.size}});
  }
  std::string bundleJson;
  const auto status = google::protobuf::util::MessageToJsonString(
      bundleMetadata_, &bundleJson, options);
  if (!status.ok()) {
    error_ = "Failed to convert bundle metadata to JSON: " + status.ToString();
    return false;
  }
  auto bundleValue = llvm::json::parse(bundleJson);
  if (!bundleValue) {
    error_ = llvm::toString(bundleValue.takeError());
    return false;
  }
  llvm::json::Value document =
      llvm::json::Object{{"format_version", MoonHeader::VERSION},
                         {"bundle_metadata", std::move(*bundleValue)},
                         {"modules", std::move(modules)},
                         {"native_archives", std::move(archives)}};
  std::ofstream out(outputPath);
  out << llvm::formatv("{0:2}", document).str() << '\n';
  out.close();
  if (!out) {
    error_ = "Failed to write moon debug JSON: " + outputPath.string();
    return false;
  }
  return true;
}

bool MoonReader::readNativeArchive(const NativeArchiveEntry& entry,
                                   std::vector<char>& out) {
  const auto found =
      std::find_if(nativeArchives_.begin(), nativeArchives_.end(),
                   [&](const NativeArchiveEntry& own) {
                     return own.offset == entry.offset &&
                            own.size == entry.size && own.name == entry.name &&
                            own.archiveSetHash == entry.archiveSetHash;
                   });
  if (found == nativeArchives_.end()) {
    return false;
  }
  return readBytes(entry.offset, entry.size, out);
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
  if (offset > bytes_->size() || size > bytes_->size() - offset) {
    error_ = "Invalid bundle byte range";
    return false;
  }
  buffer.assign(bytes_->begin() + offset, bytes_->begin() + offset + size);
  return true;
}

const moon::ModuleMetadata* MoonReader::getMetadata(
    const std::string& moduleKey) {
  const auto it = indexMap_.find(moduleKey);
  if (it == indexMap_.end()) {
    error_ = "Module not found: " + moduleKey;
    return nullptr;
  }
  return &bundleMetadata_.modules(static_cast<int>(it->second)).metadata();
}

std::string MoonReader::getBitcodeId(const std::string& moduleKey) const {
  auto it = indexMap_.find(moduleKey);
  if (it == indexMap_.end()) {
    return "";
  }
  const auto& entry = index_[it->second];
  return path_.string() + ":" + std::to_string(entry.bitcodeOffset) + ":" +
         std::to_string(entry.bitcodeSize);
}

std::string MoonReader::getTargetTriple() {
  if (index_.empty()) {
    return "";
  }
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

}  // namespace sun::moon_bundling
