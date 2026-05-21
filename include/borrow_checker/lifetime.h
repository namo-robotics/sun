// include/borrow_checker/lifetime.h
// Lifetime representation for reference safety tracking
//
// Lifetimes describe how long a reference is valid. They are inferred
// automatically by the borrow checker - Sun does not require explicit
// lifetime annotations like Rust's 'a syntax.

#pragma once

#include <cstdint>
#include <string>

namespace sun {

/// Represents the lifetime of a reference - how long it remains valid.
/// Lifetimes form a partial order based on scope containment.
///
/// Key invariants:
/// - Static lifetime outlives everything
/// - Param lifetimes outlive the function body (caller's scope)
/// - Local lifetimes are bounded by their declaring scope
/// - A reference's lifetime must not exceed its target's lifetime
class Lifetime {
 public:
  /// The kind of lifetime determines its scope behavior
  enum class Kind {
    /// 'static - lives for the entire program (e.g., string literals, globals)
    Static,

    /// Bound to a function parameter - outlives the function body
    /// since the caller owns the referenced data
    Param,

    /// Bound to a local variable - dies when variable goes out of scope
    Local,

    /// Anonymous/inferred lifetime - assigned during analysis
    Anonymous,
  };

  /// Create a static lifetime (outlives everything)
  static Lifetime static_() { return Lifetime(Kind::Static, 0, "", 0); }

  /// Create a lifetime bound to a function parameter
  /// @param paramName The parameter name (for error messages)
  static Lifetime param(const std::string& paramName) {
    return Lifetime(Kind::Param, 0, paramName, 0);
  }

  /// Create a lifetime bound to a local variable
  /// @param varName The variable name (for error messages)
  /// @param scopeDepth The scope nesting level where declared
  static Lifetime local(const std::string& varName, size_t scopeDepth) {
    return Lifetime(Kind::Local, 0, varName, scopeDepth);
  }

  /// Create an anonymous lifetime with unique ID
  /// @param id Unique identifier for this lifetime
  static Lifetime anonymous(uint32_t id) {
    return Lifetime(Kind::Anonymous, id, "", 0);
  }

  // Default constructor creates an anonymous lifetime
  Lifetime() : kind_(Kind::Anonymous), id_(0), name_(), scopeDepth_(0) {}

  // Accessors
  Kind getKind() const { return kind_; }
  uint32_t getId() const { return id_; }
  const std::string& getName() const { return name_; }
  size_t getScopeDepth() const { return scopeDepth_; }

  /// Check if this is a static lifetime
  bool isStatic() const { return kind_ == Kind::Static; }

  /// Check if this is a parameter lifetime
  bool isParam() const { return kind_ == Kind::Param; }

  /// Check if this is a local variable lifetime
  bool isLocal() const { return kind_ == Kind::Local; }

  /// Check if this is an anonymous lifetime
  bool isAnonymous() const { return kind_ == Kind::Anonymous; }

  /// Check if this lifetime outlives another.
  /// Returns true if `this` is valid everywhere `other` is valid.
  ///
  /// Outlives relationships:
  /// - Static outlives everything
  /// - Param outlives Local (param lives in caller's scope)
  /// - Local at depth N outlives Local at depth M where N < M
  /// - Same-kind lifetimes with same name/id are equal (outlive each other)
  bool outlives(const Lifetime& other) const {
    // Static outlives everything
    if (kind_ == Kind::Static) return true;
    if (other.kind_ == Kind::Static) return false;

    // Param outlives any local (caller's scope contains function body)
    if (kind_ == Kind::Param && other.kind_ == Kind::Local) return true;
    if (kind_ == Kind::Local && other.kind_ == Kind::Param) return false;

    // For same kinds, compare specifics
    if (kind_ == Kind::Param && other.kind_ == Kind::Param) {
      // Same param name means same lifetime
      return name_ == other.name_;
    }

    if (kind_ == Kind::Local && other.kind_ == Kind::Local) {
      // Outer scope (smaller depth) outlives inner scope
      if (scopeDepth_ < other.scopeDepth_) return true;
      if (scopeDepth_ > other.scopeDepth_) return false;
      // Same depth: only outlives if same variable
      return name_ == other.name_;
    }

    // Anonymous lifetimes - only equal IDs outlive each other
    if (kind_ == Kind::Anonymous && other.kind_ == Kind::Anonymous) {
      return id_ == other.id_;
    }

    // Param vs Anonymous: param outlives
    if (kind_ == Kind::Param && other.kind_ == Kind::Anonymous) return true;
    if (kind_ == Kind::Anonymous && other.kind_ == Kind::Param) return false;

    // Local vs Anonymous: depends on scope
    // Conservative: assume anonymous is at current scope
    return false;
  }

  /// Check if two lifetimes are equivalent (mutually outlive)
  bool equals(const Lifetime& other) const {
    return outlives(other) && other.outlives(*this);
  }

  /// Format lifetime for error messages
  std::string toString() const {
    switch (kind_) {
      case Kind::Static:
        return "'static";
      case Kind::Param:
        return "'" + name_ + " (parameter)";
      case Kind::Local:
        return "'" + name_ + " (local at depth " + std::to_string(scopeDepth_) +
               ")";
      case Kind::Anonymous:
        return "'_" + std::to_string(id_);
    }
    return "'unknown";
  }

 private:
  Lifetime(Kind k, uint32_t id, std::string name, size_t depth)
      : kind_(k), id_(id), name_(std::move(name)), scopeDepth_(depth) {}

  Kind kind_;
  uint32_t id_;        // For Anonymous lifetimes
  std::string name_;   // For Param and Local lifetimes
  size_t scopeDepth_;  // For Local lifetimes - nesting level
};

}  // namespace sun
