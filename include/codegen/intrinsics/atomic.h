// intrinsics/atomic.h — Atomic and synchronization intrinsic definitions
//
// Atomic intrinsics for lock-free programming and thread synchronization.
// Futex intrinsics wrap Linux futex(2) syscall for efficient waiting.

#pragma once

namespace sun {

// Atomic intrinsic identifiers are defined in the main Intrinsic enum.
// This header exists for organizational purposes and future expansion.
//
// Atomic intrinsics:
//   _atomic_cmpxchg_i32(ptr, expected, desired) -> old_value
//     Compare-and-swap with acquire-release ordering
//   _atomic_store_i32(ptr, value) -> void
//     Atomic store with release ordering
//   _atomic_load_i32(ptr) -> i32
//     Atomic load with acquire ordering
//
// Futex intrinsics (Linux-specific):
//   _futex_wait(ptr, expected) -> void
//     Block if *ptr == expected until woken
//   _futex_wake(ptr) -> void
//     Wake one thread waiting on the futex

}  // namespace sun
