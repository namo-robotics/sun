#include "moon.h"

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
  // FNV-1a 64-bit hash
  constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
  constexpr uint64_t FNV_PRIME = 1099511628211ULL;

  uint64_t hash = FNV_OFFSET;
  for (unsigned char c : data) {
    hash ^= c;
    hash *= FNV_PRIME;
  }

  // Convert to 8-char hex (32 bits)
  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(8) << (hash & 0xFFFFFFFF);
  return oss.str();
}

}  // namespace

//===----------------------------------------------------------------------===//
// FieldInfo serialization
//===----------------------------------------------------------------------===//

std::string FieldInfo::serialize() const { return name + ":" + typeSig; }

FieldInfo FieldInfo::deserialize(const std::string& data) {
  FieldInfo field;
  auto colonPos = data.find(':');
  if (colonPos != std::string::npos) {
    field.name = data.substr(0, colonPos);
    field.typeSig = data.substr(colonPos + 1);
  }
  return field;
}

//===----------------------------------------------------------------------===//
// MethodInfo serialization
//===----------------------------------------------------------------------===//

// Helper to escape special characters for single-line storage
static std::string escapeForStorage(const std::string& s) {
  std::string result;
  result.reserve(s.size() * 2);
  for (char c : s) {
    switch (c) {
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\\':
        result += "\\\\";
        break;
      default:
        result += c;
    }
  }
  return result;
}

// Helper to unescape stored string
static std::string unescapeFromStorage(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      switch (s[i + 1]) {
        case 'n':
          result += '\n';
          ++i;
          break;
        case 'r':
          result += '\r';
          ++i;
          break;
        case '\\':
          result += '\\';
          ++i;
          break;
        default:
          result += s[i];
      }
    } else {
      result += s[i];
    }
  }
  return result;
}

std::string MethodInfo::serialize() const {
  std::ostringstream oss;
  oss << name << "|" << returnTypeSig << "|" << (isStatic ? "1" : "0") << "|";
  oss << paramTypeSigs.size();
  for (size_t i = 0; i < paramTypeSigs.size(); ++i) {
    oss << "|" << paramTypeSigs[i] << ":"
        << (i < paramNames.size() ? paramNames[i] : "");
  }
  // Add type parameters
  oss << "|" << typeParams.size();
  for (const auto& tp : typeParams) {
    oss << "|" << tp;
  }
  // Add variadic info
  oss << "|" << variadicParamName << "|" << variadicConstraint;
  // Add body source (escaped to handle newlines)
  oss << "|" << escapeForStorage(bodySource);
  return oss.str();
}

MethodInfo MethodInfo::deserialize(const std::string& data) {
  MethodInfo method;
  std::istringstream iss(data);
  std::string token;

  if (std::getline(iss, token, '|')) method.name = token;
  if (std::getline(iss, token, '|')) method.returnTypeSig = token;
  if (std::getline(iss, token, '|')) method.isStatic = (token == "1");
  if (std::getline(iss, token, '|')) {
    int count = std::stoi(token);
    for (int i = 0; i < count; ++i) {
      if (std::getline(iss, token, '|')) {
        auto colonPos = token.find(':');
        if (colonPos != std::string::npos) {
          method.paramTypeSigs.push_back(token.substr(0, colonPos));
          method.paramNames.push_back(token.substr(colonPos + 1));
        } else {
          method.paramTypeSigs.push_back(token);
          method.paramNames.push_back("");
        }
      }
    }
  }
  // Deserialize type parameters
  if (std::getline(iss, token, '|')) {
    int count = std::stoi(token);
    for (int i = 0; i < count; ++i) {
      if (std::getline(iss, token, '|')) {
        method.typeParams.push_back(token);
      }
    }
  }
  // Deserialize variadic info
  if (std::getline(iss, token, '|')) method.variadicParamName = token;
  if (std::getline(iss, token, '|')) method.variadicConstraint = token;
  // Deserialize body source (escaped string - read rest of input)
  if (std::getline(iss, token)) {
    method.bodySource = unescapeFromStorage(token);
  }
  return method;
}

//===----------------------------------------------------------------------===//
// ClassInfo serialization
//===----------------------------------------------------------------------===//

std::string ClassInfo::serialize() const {
  std::ostringstream oss;
  oss << "CLASS:" << baseName << "\n";
  oss << "QUALIFIEDNAME:" << qualifiedName << "\n";
  oss << "SOURCEHASH:" << sourceHash << "\n";

  oss << "TYPEPARAMS:" << typeParams.size() << "\n";
  for (const auto& tp : typeParams) {
    oss << tp << "\n";
  }

  oss << "INTERFACES:" << interfaces.size() << "\n";
  for (const auto& iface : interfaces) {
    oss << iface << "\n";
  }

  oss << "FIELDS:" << fields.size() << "\n";
  for (const auto& field : fields) {
    oss << field.serialize() << "\n";
  }

  oss << "METHODS:" << methods.size() << "\n";
  for (const auto& method : methods) {
    oss << method.serialize() << "\n";
  }

  return oss.str();
}

ClassInfo ClassInfo::deserialize(const std::string& data) {
  ClassInfo info;
  std::istringstream iss(data);
  std::string line;

  // CLASS:baseName
  if (std::getline(iss, line) && line.substr(0, 6) == "CLASS:") {
    info.baseName = line.substr(6);
  }

  // QUALIFIEDNAME:qualifiedName
  if (std::getline(iss, line) && line.substr(0, 14) == "QUALIFIEDNAME:") {
    info.qualifiedName = line.substr(14);
  }

  // SOURCE:path (legacy) or SOURCEHASH:hash
  if (std::getline(iss, line) && line.substr(0, 11) == "SOURCEHASH:") {
    info.sourceHash = line.substr(11);
  }

  // TYPEPARAMS:count
  if (std::getline(iss, line) && line.substr(0, 11) == "TYPEPARAMS:") {
    int count = std::stoi(line.substr(11));
    for (int i = 0; i < count; ++i) {
      if (std::getline(iss, line)) {
        info.typeParams.push_back(line);
      }
    }
  }

  // INTERFACES:count
  if (std::getline(iss, line) && line.substr(0, 11) == "INTERFACES:") {
    int count = std::stoi(line.substr(11));
    for (int i = 0; i < count; ++i) {
      if (std::getline(iss, line)) {
        info.interfaces.push_back(line);
      }
    }
  }

  // FIELDS:count
  if (std::getline(iss, line) && line.substr(0, 7) == "FIELDS:") {
    int count = std::stoi(line.substr(7));
    for (int i = 0; i < count; ++i) {
      if (std::getline(iss, line)) {
        info.fields.push_back(FieldInfo::deserialize(line));
      }
    }
  }

  // METHODS:count
  if (std::getline(iss, line) && line.substr(0, 8) == "METHODS:") {
    int count = std::stoi(line.substr(8));
    for (int i = 0; i < count; ++i) {
      if (std::getline(iss, line)) {
        info.methods.push_back(MethodInfo::deserialize(line));
      }
    }
  }

  return info;
}

//===----------------------------------------------------------------------===//
// InterfaceInfo serialization
//===----------------------------------------------------------------------===//

std::string InterfaceInfo::serialize() const {
  std::ostringstream oss;
  oss << "INTERFACE:" << baseName << "\n";
  oss << "QUALIFIEDNAME:" << qualifiedName << "\n";
  oss << "METHODS:" << methods.size() << "\n";
  for (const auto& method : methods) {
    oss << method.serialize() << "\n";
  }
  return oss.str();
}

InterfaceInfo InterfaceInfo::deserialize(const std::string& data) {
  InterfaceInfo info;
  std::istringstream iss(data);
  std::string line;

  // INTERFACE:baseName
  if (std::getline(iss, line) && line.substr(0, 10) == "INTERFACE:") {
    info.baseName = line.substr(10);
  }

  // QUALIFIEDNAME:qualifiedName
  if (std::getline(iss, line) && line.substr(0, 14) == "QUALIFIEDNAME:") {
    info.qualifiedName = line.substr(14);
  }

  // METHODS:count
  if (std::getline(iss, line) && line.substr(0, 8) == "METHODS:") {
    int count = std::stoi(line.substr(8));
    for (int i = 0; i < count; ++i) {
      if (std::getline(iss, line)) {
        info.methods.push_back(MethodInfo::deserialize(line));
      }
    }
  }

  return info;
}

//===----------------------------------------------------------------------===//
// ExportedSymbol serialization
//===----------------------------------------------------------------------===//

std::string ExportedSymbol::serialize() const {
  std::ostringstream oss;
  oss << static_cast<int>(kind) << "|" << baseName << "|" << qualifiedName
      << "|" << typeSignature << "|" << (isPublic ? "1" : "0");
  return oss.str();
}

ExportedSymbol ExportedSymbol::deserialize(const std::string& data) {
  ExportedSymbol sym;
  std::istringstream iss(data);
  std::string token;

  // Kind
  if (std::getline(iss, token, '|')) {
    sym.kind = static_cast<Kind>(std::stoi(token));
  }
  // baseName
  if (std::getline(iss, token, '|')) {
    sym.baseName = token;
  }
  // qualifiedName
  if (std::getline(iss, token, '|')) {
    sym.qualifiedName = token;
  }
  // Type signature
  if (std::getline(iss, token, '|')) {
    sym.typeSignature = token;
  }
  // isPublic
  if (std::getline(iss, token, '|')) {
    sym.isPublic = (token == "1");
  }

  return sym;
}

//===----------------------------------------------------------------------===//
// ModuleMetadata serialization
//===----------------------------------------------------------------------===//

std::string ModuleMetadata::serialize() const {
  std::ostringstream oss;
  oss << "SUNLIB_META_V1\n";
  oss << "sourcehash:" << sourceHash << "\n";
  oss << "module:" << moduleName << "\n";
  oss << "version:" << version << "\n";
  oss << "hash:" << contentHash << "\n";

  oss << "deps:" << dependencies.size() << "\n";
  for (const auto& dep : dependencies) {
    oss << dep << "\n";
  }

  oss << "exports:" << exports.size() << "\n";
  for (const auto& exp : exports) {
    oss << exp.serialize() << "\n";
  }

  // Serialize classes
  oss << "classes:" << classes.size() << "\n";
  for (const auto& cls : classes) {
    std::string clsData = cls.serialize();
    oss << "CLASS_START:" << clsData.size() << "\n";
    oss << clsData;
  }

  // Serialize interfaces
  oss << "interfaces:" << interfaces.size() << "\n";
  for (const auto& iface : interfaces) {
    std::string ifaceData = iface.serialize();
    oss << "IFACE_START:" << ifaceData.size() << "\n";
    oss << ifaceData;
  }

  return oss.str();
}

ModuleMetadata ModuleMetadata::deserialize(const std::string& data) {
  ModuleMetadata meta;
  std::istringstream iss(data);
  std::string line;

  std::getline(iss, line);
  if (line != "SUNLIB_META_V1") {
    return meta;  // Invalid format
  }

  // Source hash
  std::getline(iss, line);
  if (line.substr(0, 11) == "sourcehash:") {
    meta.sourceHash = line.substr(11);
  }

  // Module name
  std::getline(iss, line);
  if (line.substr(0, 7) == "module:") {
    meta.moduleName = line.substr(7);
  }

  // Version
  std::getline(iss, line);
  if (line.substr(0, 8) == "version:") {
    meta.version = line.substr(8);
  }

  // Content hash
  std::getline(iss, line);
  if (line.substr(0, 5) == "hash:") {
    meta.contentHash = line.substr(5);
  }

  // Dependencies
  std::getline(iss, line);
  if (line.substr(0, 5) == "deps:") {
    int count = std::stoi(line.substr(5));
    for (int i = 0; i < count; ++i) {
      std::getline(iss, line);
      meta.dependencies.push_back(line);
    }
  }

  // Exports
  std::getline(iss, line);
  if (line.substr(0, 8) == "exports:") {
    int count = std::stoi(line.substr(8));
    for (int i = 0; i < count; ++i) {
      std::getline(iss, line);
      meta.exports.push_back(ExportedSymbol::deserialize(line));
    }
  }

  // Classes
  if (std::getline(iss, line) && line.substr(0, 8) == "classes:") {
    int count = std::stoi(line.substr(8));
    for (int i = 0; i < count; ++i) {
      if (std::getline(iss, line) && line.substr(0, 12) == "CLASS_START:") {
        size_t size = std::stoul(line.substr(12));
        std::string clsData(size, '\0');
        iss.read(clsData.data(), static_cast<std::streamsize>(size));
        meta.classes.push_back(ClassInfo::deserialize(clsData));
      }
    }
  }

  // Interfaces
  if (std::getline(iss, line) && line.substr(0, 11) == "interfaces:") {
    int count = std::stoi(line.substr(11));
    for (int i = 0; i < count; ++i) {
      if (std::getline(iss, line) && line.substr(0, 12) == "IFACE_START:") {
        size_t size = std::stoul(line.substr(12));
        std::string ifaceData(size, '\0');
        iss.read(ifaceData.data(), static_cast<std::streamsize>(size));
        meta.interfaces.push_back(InterfaceInfo::deserialize(ifaceData));
      }
    }
  }

  return meta;
}

//===----------------------------------------------------------------------===//
// SunLibWriter
//===----------------------------------------------------------------------===//

SunLibWriter::SunLibWriter() = default;

void SunLibWriter::addModule(llvm::Module& module,
                             const ModuleMetadata& metadata) {
  ModuleData data;

  // Serialize LLVM bitcode
  llvm::raw_string_ostream bitcodeStream(data.bitcode);
  llvm::WriteBitcodeToFile(module, bitcodeStream);
  bitcodeStream.flush();

  // Store metadata (hash will be computed in write() from combined content)
  data.metadata = metadata;

  modules_.push_back(std::move(data));
}

namespace {

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

  // Skip LLVM intrinsics
  if (name.starts_with("llvm.")) return true;
  // Skip already-prefixed symbols
  if (name.starts_with("$")) return true;
  // Skip known C runtime functions
  if (cRuntimeFunctions.count(name)) return true;
  // Skip functions starting with underscore (C runtime convention)
  if (!name.empty() && name[0] == '_') return true;
  return false;
}

// Apply content hash prefix to all symbols in a module for isolation
void applySymbolPrefix(llvm::Module& module, const std::string& prefix) {
  // Rename all functions (definitions AND declarations for cross-module refs)
  for (auto& func : module.functions()) {
    if (!func.hasName() || func.getName().empty()) continue;
    if (func.isIntrinsic()) continue;
    std::string originalName = func.getName().str();
    if (!shouldSkipRename(originalName)) {
      func.setName(prefix + "_" + originalName);
    }
  }

  // Rename all global variables
  for (auto& global : module.globals()) {
    if (!global.hasName() || global.getName().empty()) continue;
    std::string originalName = global.getName().str();
    if (!shouldSkipRename(originalName)) {
      global.setName(prefix + "_" + originalName);
    }
  }

  // Rename all struct types to avoid type merging conflicts
  // This is critical for generic class specializations which have similar
  // layouts but must remain distinct (e.g., ContiguousBuffer<i32> vs <f32>)
  for (auto* structTy : module.getIdentifiedStructTypes()) {
    if (!structTy->hasName()) continue;
    llvm::StringRef name = structTy->getName();
    // Skip LLVM internal types
    if (name.starts_with("llvm.")) continue;
    // Skip already-prefixed types
    if (name.starts_with("$")) continue;
    // Skip well-known runtime types (static_ptr, array_struct, etc.)
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

}  // namespace

bool SunLibWriter::write(const std::filesystem::path& outputPath) {
  std::ofstream out(outputPath, std::ios::binary);
  if (!out) {
    error_ = "Failed to open output file: " + outputPath.string();
    return false;
  }

  // Compute a single content hash from ALL original bitcodes in the bundle
  // This ensures all modules share the same symbol prefix and provides
  // integrity verification - if the bitcode is modified, symbols won't match
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

  // Create a context for re-parsing modules
  llvm::LLVMContext tempContext;

  for (auto& mod : modules_) {
    ModuleIndexEntry entry;
    entry.moduleKey = mod.metadata.sourceHash;

    // Re-parse the bitcode to apply symbol prefixes
    auto memBuffer = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(mod.bitcode.data(), mod.bitcode.size()),
        mod.metadata.sourceHash, false);

    auto moduleOrErr =
        llvm::parseBitcodeFile(memBuffer->getMemBufferRef(), tempContext);
    if (!moduleOrErr) {
      llvm::consumeError(moduleOrErr.takeError());
      error_ = "Failed to re-parse bitcode for symbol prefixing";
      return false;
    }

    auto& parsedModule = *moduleOrErr;

    // Apply symbol prefixes to all functions, globals, and struct types
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

    // Apply prefix to qualified names in metadata to match bitcode symbols
    // This ensures method lookups find the correct prefixed function names
    for (auto& cls : mod.metadata.classes) {
      if (!cls.qualifiedName.empty() && !cls.qualifiedName.starts_with("$")) {
        cls.qualifiedName = prefix + "_" + cls.qualifiedName;
      }
    }
    for (auto& iface : mod.metadata.interfaces) {
      if (!iface.qualifiedName.empty() &&
          !iface.qualifiedName.starts_with("$")) {
        iface.qualifiedName = prefix + "_" + iface.qualifiedName;
      }
    }
    for (auto& exp : mod.metadata.exports) {
      if (!exp.qualifiedName.empty() && !exp.qualifiedName.starts_with("$")) {
        exp.qualifiedName = prefix + "_" + exp.qualifiedName;
      }
    }

    // Set shared bundle hash and serialize metadata
    mod.metadata.contentHash = bundleHash;
    std::string metadataStr = mod.metadata.serialize();

    // Write metadata
    entry.metadataOffset = static_cast<uint64_t>(out.tellp());
    entry.metadataSize = metadataStr.size();
    out.write(metadataStr.data(),
              static_cast<std::streamsize>(metadataStr.size()));

    index.push_back(entry);
  }

  // Write index
  header.indexOffset = static_cast<uint32_t>(out.tellp());

  // Index format: count, then for each entry:
  // keyLen(4), key, bitcodeOffset(8), bitcodeSize(8), metadataOffset(8),
  // metadataSize(8)
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
  // Only accept V1 format
  if (header.version != 1) {
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

const ModuleMetadata* SunLibReader::getMetadata(const std::string& moduleKey) {
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

  // Read metadata
  std::vector<char> buffer;
  if (!readBytes(entry.metadataOffset, entry.metadataSize, buffer)) {
    return nullptr;
  }

  std::string metadataStr(buffer.begin(), buffer.end());
  metadataCache_[moduleKey] = ModuleMetadata::deserialize(metadataStr);

  return &metadataCache_[moduleKey];
}

std::unique_ptr<llvm::Module> SunLibReader::loadModule(
    const std::string& moduleKey, llvm::LLVMContext& context) {
  // Find index entry
  auto it = indexMap_.find(moduleKey);
  if (it == indexMap_.end()) {
    error_ = "Module not found in index: " + moduleKey + " (available: ";
    for (const auto& [k, v] : indexMap_) {
      error_ += k + ", ";
    }
    error_ += ")";
    return nullptr;
  }

  const auto& entry = index_[it->second];

  // Read bitcode
  std::vector<char> buffer;
  if (!readBytes(entry.bitcodeOffset, entry.bitcodeSize, buffer)) {
    error_ = "Failed to read bitcode at offset " +
             std::to_string(entry.bitcodeOffset) + " size " +
             std::to_string(entry.bitcodeSize);
    return nullptr;
  }

  // Parse bitcode
  auto memBuffer = llvm::MemoryBuffer::getMemBuffer(
      llvm::StringRef(buffer.data(), buffer.size()), moduleKey,
      false  // RequiresNullTerminator
  );

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
