// config.h - Compile-time configuration for Sun language features
#pragma once

namespace sun {

/// Compile-time configuration for Sun language behavior.
/// Modify these settings to enable/disable experimental features.
struct Config {
  // ============================================================
  // Type System
  // ============================================================

  /// If true, compound types (classes, interfaces) cannot be passed by value
  /// to functions - they must use `ref T` or pointer types.
  /// If false, compound types are passed by value with move semantics.
  static constexpr bool REQUIRE_REF_FOR_COMPOUND_PARAMS = false;

  /// If true, reference types (ref T) cannot be stored in class fields.
  /// References are meant for parameter passing, not storage.
  static constexpr bool FORBID_REF_FIELDS_IN_CLASSES = false;

  /// If true, functions cannot return reference types (ref T).
  /// Returning references without lifetimes risks dangling references.
  static constexpr bool FORBID_REF_RETURNS = false;

  // ============================================================
  // Borrow Checker
  // ============================================================

  /// Simplification: treat all refs as mutable borrows.
  /// When true, shared (immutable) borrows are not distinguished from mutable.
  static constexpr bool TREAT_ALL_BORROWS_AS_MUTABLE = true;

  /// If true, disallow mutating a variable while it has active borrows
  /// (Rust-style strict checking). If false, allow mutation (Sun's default).
  static constexpr bool STRICT_MUTATION_CHECKING = false;

  // ============================================================
  // Memory Management
  // ============================================================

  /// If true, class instances have deinit() called automatically at scope exit.
  static constexpr bool AUTO_DEINIT_CLASSES = true;

  // ============================================================
  // Debug / Development
  // ============================================================

  /// If true, dump generated LLVM IR to stdout during JIT execution.
  static constexpr bool DUMP_IR_ON_JIT = true;
};

}  // namespace sun
