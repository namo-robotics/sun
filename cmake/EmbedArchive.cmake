# Embeds a binary archive for linking, preserving the output timestamp when unchanged.
file(READ "${INPUT_FILE}" bytes HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
set(source "// Generated archive contents; do not edit.
#include <llvm/ADT/StringRef.h>
namespace sun::driver {
namespace {
const unsigned char archive[] = {${bytes}};
}
llvm::StringRef embeddedExceptionRuntime() {
  return {reinterpret_cast<const char*>(archive), sizeof(archive)};
}
}
")
file(CONFIGURE OUTPUT "${OUTPUT_FILE}" CONTENT "${source}" @ONLY)
