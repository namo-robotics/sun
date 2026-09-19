// debug_info_builder.h — DWARF debug metadata emission for -g builds
//
// Mirrors the LLVMTypeResolver idiom: a cache plus resolve(TypePtr), but
// producing llvm::DIType nodes. Every public method is a no-op when the
// builder is disabled, so builds without -g generate identical IR.

#pragma once

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "semantic_analysis/types.h"
#include "support/position.h"

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {
using sun::semantic_analysis::TypePtr;

/** Builds source-level debug metadata for generated LLVM code. */
class DebugInfoBuilder {
  llvm::Module* module_ = nullptr;
  std::unique_ptr<llvm::DIBuilder> di_;
  llvm::DICompileUnit* cu_ = nullptr;
  std::map<std::string, llvm::DIFile*> fileCache_;
  std::map<const sun::semantic_analysis::Type*, llvm::DIType*> typeCache_;
  // Innermost-last stack of subprogram / lexical block scopes for the
  // function currently being emitted (mirrors CodegenVisitor's scopes).
  std::vector<llvm::DIScope*> scopeStack_;
  bool finalized_ = false;
  bool optimized_ = false;

 public:
  /**
   * Build debug metadata, recording whether the generated code is optimized.
   */
  DebugInfoBuilder(llvm::Module* module, bool enabled, bool optimized = false);

  /** Reports whether debug metadata generation is enabled. */
  bool enabled() const { return di_ != nullptr; }

  /** Returns debug metadata for a source file, creating it when necessary. */
  llvm::DIFile* getFile(const std::optional<std::string>& path);

  /**
   * Create a DISubprogram for a defined function, attach it, push its scope,
   * and clear the builder's inherited debug location.
   */
  llvm::DISubprogram* enterFunction(llvm::IRBuilderBase& builder,
                                    llvm::Function* func,
                                    const std::string& name,
                                    const sun::support::Position& loc);
  /**
   * Pop scopes down through this function's subprogram and finalize it.
   */
  void exitFunction(llvm::Function* func);

  /**
   * Open a DILexicalBlock under the current scope so variables declared in
   * source blocks (if/else, loops, try/catch) get block-accurate visibility.
   * Returns true when a block was pushed (caller must pair with
   * popLexicalBlock); false when disabled or the insert point is in a
   * function without a subprogram.
   */
  bool pushLexicalBlock(llvm::IRBuilderBase& builder,
                        const sun::support::Position& loc);
  /** Leaves the current lexical block in debug metadata. */
  void popLexicalBlock();

  /**
   * Set the builder's debug location for the expression about to be emitted:
   * scoped to the insert-point function's subprogram, or cleared when that
   * function has none. Call once per expression dispatch.
   */
  void attachExpressionLocation(llvm::IRBuilderBase& builder,
                                const sun::support::Position& loc);

  /**
   * Drop the builder's debug location. Required when switching the insert
   * point into a function that has no subprogram (wrappers, init functions):
   * such functions must carry no debug locations at all.
   */
  void clearLocation(llvm::IRBuilderBase& builder);

  /** Emits debug metadata identifying a function parameter and its storage. */
  void declareParameter(llvm::IRBuilderBase& builder, llvm::AllocaInst* alloca,
                        const std::string& name, const TypePtr& type,
                        const sun::support::Position& loc, unsigned argNo,
                        bool artificial = false);
  /**
   * 'this' receiver: a pointer to the class struct, marked artificial.
   */
  void declareThisParameter(llvm::IRBuilderBase& builder,
                            llvm::AllocaInst* alloca, const TypePtr& classType);
  /** Emits debug metadata identifying a local variable and its storage. */
  void declareLocal(llvm::IRBuilderBase& builder, llvm::AllocaInst* alloca,
                    const std::string& name, const TypePtr& type,
                    const sun::support::Position& loc);

  /** Returns debug metadata describing the supplied semantic type. */
  llvm::DIType* resolveType(const TypePtr& type);

  /** Completes debug metadata construction before the module is emitted. */
  void finalize();

  /**
   * Remove all debug metadata a module carries (e.g. inherited from linked
   * .moon bundles built with -g), including the module flags that would make
   * emitObjectFile treat it as a debug build.
   */
  static void stripFromModule(llvm::Module& module);

 private:
  /** Creates the debug compilation unit when one is not already present. */
  llvm::DICompileUnit* ensureCompileUnit(
      const std::optional<std::string>& hint);
  /** Returns the debug scope active within the supplied function. */
  llvm::DILocalScope* currentLocalScope(llvm::Function* func) const;
  /** Constructs debug metadata describing a semantic type. */
  llvm::DIType* resolveTypeImpl(const sun::semantic_analysis::Type& type);
  /** Creates debug metadata for a pointer to the supplied type. */
  llvm::DIType* pointerTo(llvm::DIType* pointee);
  /** Creates debug metadata describing the fields of an LLVM structure. */
  llvm::DIType* structFor(
      const std::string& name, llvm::StructType* st,
      const std::vector<std::pair<std::string, llvm::DIType*>>& members);
  /** Associates a debug variable with its generated storage and source scope. */
  void declareVariable(llvm::IRBuilderBase& builder, llvm::AllocaInst* alloca,
                       llvm::DILocalVariable* var,
                       const sun::support::Position& loc,
                       llvm::DILocalScope* scope);
};

}  // namespace sun::codegen
