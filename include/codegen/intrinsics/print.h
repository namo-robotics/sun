// intrinsics/print.h — Print intrinsic definitions
//
// Low-level print intrinsics for debugging and output.
// These write directly to stdout using syscalls.

#pragma once

namespace sun {

// Print intrinsic identifiers are defined in the main Intrinsic enum.
// This header exists for organizational purposes and future expansion.
//
// Print intrinsics:
//   _print_i32(value) -> void       Print 32-bit integer
//   _print_i64(value) -> void       Print 64-bit integer
//   _print_f64(value) -> void       Print 64-bit float
//   _print_newline() -> void        Print newline character
//   _print_bytes(ptr, len) -> void  Print len bytes from ptr
//   _println_str(str) -> void       Print string with newline

}  // namespace sun
