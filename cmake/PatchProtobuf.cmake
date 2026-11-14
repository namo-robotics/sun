# Apply warning fixes to the pinned protobuf release without disabling diagnostics.

# Replace a known upstream fragment, accepting an already patched source tree.
function(replace_protobuf_fragment relative_path original replacement)
    set(path "${SOURCE_DIR}/${relative_path}")
    file(READ "${path}" contents)
    string(FIND "${contents}" "${replacement}" patched_position)
    if(NOT patched_position EQUAL -1)
        return()
    endif()
    string(FIND "${contents}" "${original}" original_position)
    if(original_position EQUAL -1)
        message(FATAL_ERROR "Pinned protobuf patch no longer matches ${relative_path}")
    endif()
    string(REPLACE "${original}" "${replacement}" contents "${contents}")
    file(WRITE "${path}" "${contents}")
endfunction()

# The dependency builds with the policies required by current CMake releases.
replace_protobuf_fragment(CMakeLists.txt
    "cmake_minimum_required(VERSION 3.5)"
    "cmake_minimum_required(VERSION 3.10)")

# An escaped byte needs four characters and a terminator. Check the full space
# before writing, and use the bounded API supported by current macOS SDKs.
replace_protobuf_fragment(src/google/protobuf/stubs/strutil.cc
    "if (dest_len - used < 4) // need space for 4 letter escape"
    "if (dest_len - used < 5) // Include the terminating null byte.")
replace_protobuf_fragment(src/google/protobuf/stubs/strutil.cc
    "sprintf(dest + used, (use_hex ?"
    "snprintf(dest + used, dest_len - used, (use_hex ?")

# Fatal logging throws or aborts, but its return type does not express that.
# Keep AlignFail's noreturn contract explicit in assertion-enabled builds.
replace_protobuf_fragment(src/google/protobuf/generated_message_tctable_impl.h
    "#include <cstdint>"
    "#include <cstdint>\n#include <cstdlib>")
replace_protobuf_fragment(src/google/protobuf/generated_message_tctable_impl.h
    "GOOGLE_LOG(FATAL) << \"Unaligned (\" << align << \") access at \" << address;"
    "GOOGLE_LOG(FATAL) << \"Unaligned (\" << align << \") access at \" << address;\n  std::abort();")
