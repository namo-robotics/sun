// intrinsics/atomic.h — Atomic and synchronization intrinsic definitions
//
// Atomic intrinsics for lock-free programming and thread synchronization.
// Futex intrinsics wrap Linux futex(2) syscall for efficient waiting.

#pragma once

namespace sun {

// Atomic intrinsic identifiers are defined in the main Intrinsic enum.
// This header exists for organizational purposes and future expansion.
//
// Atomic intrinsics use i32, i64, or u64 suffixes:
//   _atomic_cmpxchg_*(ptr, expected, desired) -> old_value
//     Compare-and-swap with acquire-release success and acquire failure
//   _atomic_store_*(ptr, value) -> void
//     Atomic store with release ordering
//   _atomic_load_*(ptr) -> value
//     Atomic load with acquire ordering
//   _atomic_fetch_add_*(ptr, delta) -> old_value
//     Atomic add with acquire-release ordering
//   _atomic_fetch_sub_*(ptr, delta) -> old_value
//     Atomic subtract with acquire-release ordering. The release half is what
//     a reference count needs: the owner that drives the count to zero sees
//     every other owner's writes before it runs the drop.
//   _atomic_fence_acquire() / _atomic_fence_release() -> void
//     Explicit one-way memory fences
//
// Futex intrinsics (Linux-specific):
//   _futex_wait(ptr, expected) -> void
//     Block if *ptr == expected until woken
//   _futex_wake(ptr) -> void
//     Wake one thread waiting on the futex

}  // namespace sun
