#pragma once

#include <llvm/IR/Module.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "moon.pb.h"

namespace sun {

// =============================================================================
// Binary format structures
// =============================================================================

/// Binary header for .moon format
struct MoonHeader {
  static constexpr uint32_t MAGIC = 0x53554E4C;  // "SUNL"
  static constexpr uint32_t VERSION = 8;  // V8: symbols spelled with the
                                          // bundle hash by the compiler, so
                                          // older bundles' symbols no longer
                                          // match what importers compute

  uint32_t magic = MAGIC;
  uint32_t version = VERSION;
  // The index carries its own entry count, so the number of modules is not
  // repeated here. 64-bit because it addresses anywhere in the payload.
  uint64_t indexOffset = 0;
};

/// Index entry for a module in the bundle
struct ModuleIndexEntry {
  std::string moduleKey;  // Source hash used as module identifier
  uint64_t bitcodeOffset;
  uint64_t bitcodeSize;
  uint64_t metadataOffset;
  uint64_t metadataSize;
};

/// A native static library (`.a`) carried inside the bundle, so a moon that
/// binds a C library brings that library's code with it. Programs importing
/// the bundle link (AOT) or load (JIT) these without naming -l flags.
struct NativeArchiveEntry {
  std::string name;  // file name, e.g. "libssl.a"
  uint64_t offset = 0;
  uint64_t size = 0;
};

// =============================================================================
// Writer and Reader classes
// =============================================================================

/// FNV-1a hash of `data` as 8 hex characters. Bundle hashes are made from
/// this; it keeps the `$hash$_` symbol prefix short enough to read in IR.
std::string computeContentHash(const std::string& data);

/// Creates .moon bundle files containing multiple modules
class MoonWriter {
 public:
  /// @param bundleHash The bundle's content hash, chosen before its code was
  /// compiled so the compiler could spell every exported symbol with the
  /// `$hash$_` prefix. Recorded in each module's metadata for importers.
  explicit MoonWriter(std::string bundleHash);

  /// Add a compiled module to the bundle
  /// @param module The compiled LLVM module
  /// @param metadata Module metadata (protobuf) including AST nodes
  void addModule(llvm::Module& module, const moon::ModuleMetadata& metadata);

  /// Carry a native static library inside the bundle
  /// @param name File name recorded in the bundle (e.g. "libssl.a")
  /// @param data Raw archive contents
  void addNativeArchive(std::string name, std::string data);

  /// Write the bundle to disk
  /// @param outputPath Path to write the .moon file
  /// @return true on success
  bool write(const std::filesystem::path& outputPath);

  /// Get error message if write failed
  const std::string& getError() const { return error_; }

 private:
  struct ModuleData {
    size_t blobIndex;  // index into blobs_
    moon::ModuleMetadata metadata;
  };

  // Bundles normally share one code image across every exported module (the
  // whole bundle is compiled into a single LLVM module), so the bitcode is
  // held once and referenced by index rather than copied per module.
  std::vector<std::string> blobs_;
  std::unordered_map<const llvm::Module*, size_t> blobIndexByModule_;

  std::vector<ModuleData> modules_;
  std::vector<std::pair<std::string, std::string>> nativeArchives_;
  std::string bundleHash_;
  std::string error_;
};

/// Reads .moon bundle files and extracts individual modules
class MoonReader {
 public:
  /// Open a .moon bundle file
  /// @param path Path to the .moon file
  /// @return Reader instance, or nullptr on failure
  static std::unique_ptr<MoonReader> open(const std::filesystem::path& path);

  /// Check if a module exists in this bundle
  /// @param moduleKey The module key (source hash)
  bool hasModule(const std::string& moduleKey) const;

  /// List all modules in the bundle
  std::vector<std::string> listModules() const;

  /// Get metadata for a module without loading bitcode
  /// @param moduleKey The module key (source hash)
  /// @return Protobuf metadata, or nullptr if not found
  const moon::ModuleMetadata* getMetadata(const std::string& moduleKey);

  /// Load a module's LLVM bitcode
  /// @param moduleKey The module key (source hash)
  /// @param context LLVM context to create module in
  /// @return LLVM module, or nullptr on failure
  std::unique_ptr<llvm::Module> loadModule(const std::string& moduleKey,
                                           llvm::LLVMContext& context);

  /// Identity of the bitcode region backing a module. Modules that share a
  /// code image share this string, so a caller can scan the image once
  /// instead of once per module. Empty if the module is unknown.
  std::string getBitcodeId(const std::string& moduleKey) const;

  /// Get error message if an operation failed
  const std::string& getError() const { return error_; }

  /// Get the path to this bundle file
  const std::filesystem::path& getPath() const { return path_; }

  /// The target the bundle's bitcode was compiled for (LLVM triple), taken
  /// from the first module's metadata. Empty for pre-V4 legacy bundles.
  std::string getTargetTriple();

  /// Native static libraries carried by this bundle, in link order
  const std::vector<NativeArchiveEntry>& getNativeArchives() const {
    return nativeArchives_;
  }

  /// Read one carried archive's bytes
  /// @return false if the name is unknown or the read failed
  bool readNativeArchive(const std::string& name, std::vector<char>& out);

 private:
  MoonReader() = default;

  std::filesystem::path path_;
  std::vector<ModuleIndexEntry> index_;
  std::unordered_map<std::string, size_t> indexMap_;
  std::unordered_map<std::string, moon::ModuleMetadata> metadataCache_;
  std::vector<NativeArchiveEntry> nativeArchives_;
  std::string error_;

  /// Read raw bytes from the bundle file
  bool readBytes(uint64_t offset, uint64_t size, std::vector<char>& buffer);
};

/// Get symbol prefix for protobuf metadata
inline std::string getSymbolPrefix(const moon::ModuleMetadata& metadata) {
  return metadata.content_hash().empty() ? ""
                                         : "$" + metadata.content_hash() + "$";
}

}  // namespace sun
