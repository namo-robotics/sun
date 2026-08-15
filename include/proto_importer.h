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
  // `import` statements resolve against `importDirs` in order (the manifest's
  // base directory, then SUN_PATH entries) and the proto's own directory.
  // Throws SunError with file:line:col diagnostics on schema errors and on
  // unsupported constructs.
  static SynthesizedProtoModule import(
      const std::string& protoPath,
      const std::vector<std::string>& importDirs);
};

}  // namespace sun
