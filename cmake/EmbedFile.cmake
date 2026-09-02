# EmbedFile.cmake — Embeds a text file into a C++ header as a raw string
# literal, so the compiler binary stays self-contained. Only rewrites
# OUTPUT_FILE when the content changed, keeping depending targets from
# rebuilding needlessly.
#
# Expects: -DINPUT_FILE=<text file> -DOUTPUT_FILE=<header to write>
#          -DVARIABLE_NAME=<C++ identifier for the literal>

file(READ "${INPUT_FILE}" content)
get_filename_component(input_name "${INPUT_FILE}" NAME)
set(header "// Generated from ${input_name} by EmbedFile.cmake - do not edit.
#pragma once

inline constexpr char ${VARIABLE_NAME}[] = R\"SUNEMBED(${content})SUNEMBED\";
")

if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" existing)
else()
    set(existing "")
endif()
if(NOT header STREQUAL existing)
    file(WRITE "${OUTPUT_FILE}" "${header}")
endif()
