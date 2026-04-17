#pragma once

#include <llvm/IR/Module.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sun {

/// Field in a class or struct
struct FieldInfo {
  std::string name;
  std::string typeSig;  // Serialized type

  std::string serialize() const;
  static FieldInfo deserialize(const std::string& data);
};

/// Method signature in a class or interface
struct MethodInfo {
  std::string name;
  std::string returnTypeSig;
  std::vector<std::string> paramTypeSigs;
  std::vector<std::string> paramNames;
  std::vector<std::string> typeParams;  // Generic type parameters: <T, U>
  std::string variadicParamName;   // Name of variadic param (empty if none)
  std::string variadicConstraint;  // Type constraint for variadic (e.g.,
                                   // "_init_args<T>")
  std::string bodySource;  // Source code of method body (for generic methods)
  bool isStatic = false;

  std::string serialize() const;
  static MethodInfo deserialize(const std::string& data);

  bool isGeneric() const { return !typeParams.empty(); }
};

/// Class definition
struct ClassInfo {
  std::string name;
  std::string sourceFile;  // Original source file (for generic method parsing)
  std::vector<std::string> typeParams;  // Generic type parameters
  std::vector<std::string> interfaces;  // Implemented interfaces
  std::vector<FieldInfo> fields;
  std::vector<MethodInfo> methods;

  std::string serialize() const;
  static ClassInfo deserialize(const std::string& data);
};

/// Interface definition
struct InterfaceInfo {
  std::string name;
  std::vector<MethodInfo> methods;

  std::string serialize() const;
  static InterfaceInfo deserialize(const std::string& data);
};

/// Exported symbol from a Sun library module
struct ExportedSymbol {
  enum class Kind : uint8_t {
    Function = 0,
    Type = 1,
    Constant = 2,
    Class = 3,
    Interface = 4
  };

  Kind kind;
  std::string name;           // Source name: "abs"
  std::string mangledName;    // LLVM name: "std_math_abs"
  std::string typeSignature;  // Serialized type: "(i32) -> i32"
  bool isPublic = true;

  std::string serialize() const;
  static ExportedSymbol deserialize(const std::string& data);
};

/// Metadata for a single module within a .moon bundle
struct ModuleMetadata {
  std::string importPath;                 // "stdlib/allocator.sun"
  std::string version;                    // "1.0.0"
  std::vector<std::string> dependencies;  // Other modules this imports
  std::vector<ExportedSymbol> exports;
  std::vector<ClassInfo> classes;         // Class definitions
  std::vector<InterfaceInfo> interfaces;  // Interface definitions

  std::string serialize() const;
  static ModuleMetadata deserialize(const std::string& data);
};

/// Binary header for .moon format
struct SunLibHeader {
  static constexpr uint32_t MAGIC = 0x53554E4C;  // "SUNL"
  static constexpr uint32_t VERSION = 2;         // Bumped for new format

  uint32_t magic = MAGIC;
  uint32_t version = VERSION;
  uint32_t moduleCount = 0;
  uint32_t indexOffset = 0;  // Offset to module index
};

/// Index entry for a module in the bundle
struct ModuleIndexEntry {
  std::string importPath;
  uint64_t bitcodeOffset;
  uint64_t bitcodeSize;
  uint64_t metadataOffset;
  uint64_t metadataSize;
};

/// Creates .moon bundle files containing multiple modules
class SunLibWriter {
 public:
  SunLibWriter();

  /// Add a compiled module to the bundle
  /// @param importPath The import path (e.g., "stdlib/allocator.sun")
  /// @param module The compiled LLVM module
  /// @param metadata Module metadata including exports
  void addModule(const std::string& importPath, llvm::Module& module,
                 const ModuleMetadata& metadata);

  /// Write the bundle to disk
  /// @param outputPath Path to write the .moon file
  /// @return true on success
  bool write(const std::filesystem::path& outputPath);

  /// Get error message if write failed
  const std::string& getError() const { return error_; }

 private:
  struct ModuleData {
    std::string importPath;
    std::string bitcode;
    std::string metadata;
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
  /// @param importPath The import path (e.g., "stdlib/allocator.sun")
  bool hasModule(const std::string& importPath) const;

  /// List all modules in the bundle
  std::vector<std::string> listModules() const;

  /// Get metadata for a module without loading bitcode
  /// @param importPath The import path
  /// @return Metadata, or nullptr if not found
  const ModuleMetadata* getMetadata(const std::string& importPath);

  /// Load a module's LLVM bitcode
  /// @param importPath The import path
  /// @param context LLVM context to create module in
  /// @return LLVM module, or nullptr on failure
  std::unique_ptr<llvm::Module> loadModule(const std::string& importPath,
                                           llvm::LLVMContext& context);

  /// Get error message if an operation failed
  const std::string& getError() const { return error_; }

  /// Get the path to this bundle file
  const std::filesystem::path& getPath() const { return path_; }

 private:
  SunLibReader() = default;

  std::filesystem::path path_;
  std::vector<ModuleIndexEntry> index_;
  std::unordered_map<std::string, size_t> indexMap_;  // importPath -> index
  std::unordered_map<std::string, ModuleMetadata> metadataCache_;
  std::string error_;

  /// Read raw bytes from the bundle file
  bool readBytes(uint64_t offset, uint64_t size, std::vector<char>& buffer);
};

}  // namespace sun
