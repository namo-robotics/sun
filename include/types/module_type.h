/** Describes references to modules during name resolution. */
#pragma once

#include "semantic_analysis/visibility.h"
#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * Module/namespace reference type
 * Used in semantic analysis when accessing module-scoped variables/functions
 * e.g., in "mod_x.mod_y.a", mod_x and mod_x.mod_y have ModuleType
 */
class ModuleType : public Type {
  std::string modulePath;  // e.g., "mod_x" or "$hash$_mod_x"

 public:
  /** Creates a type describing a reference to a module path. */
  explicit ModuleType(std::string path) : modulePath(std::move(path)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Module;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the module path stored by this object. */
  const std::string& getModulePath() const { return modulePath; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "module<" + modulePath + ">"; }

  /**
   * The path as source spells it, without the "$hash$" scope a moon import
   * adds
   */
  std::string toDisplayString() const override {
    return "module<" + sun::semantic_analysis::displayModulePath(modulePath) +
           ">";
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (!other.isModule()) return false;
    return modulePath == static_cast<const ModuleType&>(other).modulePath;
  }

  /**
   * Module types don't have LLVM representation - they're resolved at compile
   * time
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return nullptr;
  }
};

}  // namespace sun::types
