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
struct SunLibHeader {
  static constexpr uint32_t MAGIC = 0x53554E4C;  // "SUNL"
  static constexpr uint32_t VERSION = 2;         // V2: Protobuf metadata format

  uint32_t magic = MAGIC;
  uint32_t version = VERSION;
  uint32_t moduleCount = 0;
  uint32_t indexOffset = 0;  // Offset to module index
};

/// Index entry for a module in the bundle
struct ModuleIndexEntry {
  std::string moduleKey;  // Source hash used as module identifier
  uint64_t bitcodeOffset;
  uint64_t bitcodeSize;
  uint64_t metadataOffset;
  uint64_t metadataSize;
};

// =============================================================================
// Writer and Reader classes
// =============================================================================

/// Creates .moon bundle files containing multiple modules
class SunLibWriter {
 public:
  SunLibWriter();

  /// Add a compiled module to the bundle
  /// @param module The compiled LLVM module
  /// @param metadata Module metadata (protobuf) including AST nodes
  void addModule(llvm::Module& module, const moon::ModuleMetadata& metadata);

  /// Write the bundle to disk
  /// @param outputPath Path to write the .moon file
  /// @return true on success
  bool write(const std::filesystem::path& outputPath);

  /// Get error message if write failed
  const std::string& getError() const { return error_; }

 private:
  struct ModuleData {
    std::string bitcode;
    moon::ModuleMetadata metadata;
  };

  std::vector<ModuleData> modules_;
  std::string error_;
};

/// Reads .moon bundle files and extracts individual modules
class SunLibReader {
 public:
  /// Open a .moon bundle file
  /// @param path Path to the .moon file
  /// @return Reader instance, or nullptr on failure
  static std::unique_ptr<SunLibReader> open(const std::filesystem::path& path);

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

  /// Get error message if an operation failed
  const std::string& getError() const { return error_; }

  /// Get the path to this bundle file
  const std::filesystem::path& getPath() const { return path_; }

 private:
  SunLibReader() = default;

  std::filesystem::path path_;
  std::vector<ModuleIndexEntry> index_;
  std::unordered_map<std::string, size_t> indexMap_;
  std::unordered_map<std::string, moon::ModuleMetadata> metadataCache_;
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
