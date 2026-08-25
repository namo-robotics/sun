// proto_importer.h — Native protobuf import: parse a .proto schema in-process
// (google::protobuf::compiler::Importer) and synthesize ordinary Sun source
// for it — one class per message with encode/decode, enums, oneofs — inside
// a module named after the proto package. The synthesized source is compiled
// with the program like any other file; no libprotobuf ends up in the output.

#pragma once

#include <string>
#include <vector>

namespace sun {

struct SynthesizedProtoModule {
  std::string sunSource;   // generated Sun text
  std::string pseudoPath;  // "<proto:schemas/telemetry.proto>" for diagnostics
  std::string moduleName;  // "namo.telemetry" (proto package)
};

class ProtoImporter {
 public:
  // Import `protoPath` (already resolved to a real file). Proto-level
  // `import` statements resolve against the proto's own directory, then
  // `importDirs` in order. Throws SunError with file:line:col diagnostics on
  // schema errors and on unsupported constructs.
  static SynthesizedProtoModule import(
      const std::string& protoPath, const std::vector<std::string>& importDirs);

  // The standard import directories for schemas listed in a manifest: the
  // manifest's base directory followed by the SUN_PATH entries.
  static std::vector<std::string> importDirsFor(const std::string& baseDir);

  // Import every schema of a manifest (resolved paths) with
  // importDirsFor(baseDir).
  static std::vector<SynthesizedProtoModule> importAll(
      const std::vector<std::string>& protoFiles, const std::string& baseDir);
};

}  // namespace sun
