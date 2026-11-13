# Embeds a binary archive for linking, preserving the output timestamp when unchanged.
file(READ "${INPUT_FILE}" bytes HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
# Keep cross linking self-contained without shipping target-specific archives.
set(runtime_source "")
foreach(path IN ITEMS include/abi/exception_layout.h include/runtime/exceptions.h
        include/runtime/panic.h src/runtime/exceptions.cpp src/runtime/panic.cpp)
    file(READ "${SOURCE_DIR}/${path}" part)
    string(REGEX REPLACE "#pragma once\n|#include \"[^\"]+\"\n" "" part "${part}")
    string(APPEND runtime_source "${part}\n")
endforeach()
set(source "// Generated archive contents; do not edit.
#include <llvm/ADT/StringRef.h>
namespace sun::driver {
namespace {
const unsigned char archive[] = {${bytes}};
}
llvm::StringRef embeddedExceptionRuntime() {
  return {reinterpret_cast<const char*>(archive), sizeof(archive)};
}
llvm::StringRef embeddedExceptionRuntimeSource() {
  return R\"SUN_RUNTIME(${runtime_source})SUN_RUNTIME\";
}
}
")
file(CONFIGURE OUTPUT "${OUTPUT_FILE}" CONTENT "${source}" @ONLY)
