#pragma once

// codegen_state.h — What one codegen run holds
//
// Two objects here are easy to confuse, so the split is deliberate:
//
//   CodegenContext (codegen.h)  the LLVM plumbing — context, IR builder,
//                               module, pass managers, JIT. The driver creates
//                               it and it outlives any one codegen run.
//   CodegenState   (this file)  what emitting a program needs on top of that —
//                               the type registry, the Sun-to-LLVM type
//                               resolver, DWARF emission, and where the
//                               emitter currently is.
//
// Every codegen component takes CodegenState by reference, the way every
// semantic component takes SemanticContext
// (semantic_analysis/semantic_context.h). The state answers only positional
// questions: what module is being built, what function is being written, what
// `this` means right now. It never walks the AST — that needs CodegenVisitor,
// which depends on this and not the other way round.

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <memory>

#include "codegen/codegen.h"
#include "codegen/debug_info_builder.h"
#include "codegen/llvm_type_resolver.h"
#include "semantic_analysis/types.h"

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {

/**
 * The function body currently being emitted: its receiver, and what its
 * return statements are allowed to do. Nested emission (a method body inside
 * a class definition, a lambda inside a function) saves and restores this
 * through the guards on CodegenState rather than by hand.
 */
struct FunctionFrame {
  // Current 'this' pointer; set while compiling a method body
  llvm::Value* thisPtr = nullptr;

  // Class owning the method being compiled, for method name resolution
  std::shared_ptr<sun::semantic_analysis::ClassType> currentClass = nullptr;

  // True when the function is declared to return errors, so division and
  // modulo take the checked path and calls may unwind
  bool canError = false;

  // True while emitting a function declared to return `ref T`; reference
  // returns must hand back the referent's address
  bool returnsRef = false;

  // The T in an error union { i1, T }
  llvm::Type* valueType = nullptr;
};

/**
 * The state shared by every part of a codegen run. Held by value on
 * CodegenVisitor and handed to each component by reference.
 */
class CodegenState {
 public:
  // The LLVM plumbing this run emits into
  CodegenContext& ctx;

  // The module being built; always ctx.mainModule.get()
  llvm::Module* module;

  // Class and interface types, shared with the semantic analyzer
  std::shared_ptr<sun::semantic_analysis::TypeRegistry> typeRegistry;

  // sun::semantic_analysis::Type -> llvm::Type conversion, with its own cache
  LLVMTypeResolver typeResolver;

  // DWARF metadata emission; no-op unless -g
  sun::codegen::DebugInfoBuilder debugInfo;

  // Where the emitter currently is
  FunctionFrame frame;

  /** Connects LLVM generation state to the semantic type registry. */
  CodegenState(CodegenContext& ctx,
               std::shared_ptr<sun::semantic_analysis::TypeRegistry> registry)
      : ctx(ctx),
        module(ctx.mainModule.get()),
        typeRegistry(std::move(registry)),
        typeResolver(ctx.getContext(), &ctx.mainModule->getDataLayout()),
        debugInfo(ctx.mainModule.get(), ctx.debugInfoEnabled(),
                  ctx.optimizationEnabled()) {}

  /** Connects LLVM generation state to the semantic type registry. */
  CodegenState(const CodegenState&) = delete;
  /** Disallows assignment so ownership and object identity cannot be duplicated. */
  CodegenState& operator=(const CodegenState&) = delete;

  /** Derive the linker spelling for an analyzed declaration and emission role.
   */
  std::string declarationSymbol(sun::semantic_analysis::DeclarationId id,
                                const std::string& role = "function") const {
    const auto& table = typeRegistry->declarations;
    const auto& record = table.get(id);
    if (role == "function" && record.name == "main" && !record.owner)
      return "main";
    return sun::semantic_analysis::PortableDeclarationKey::fromDeclaration(
               id, table)
        .symbol(role);
  }

  /** Provides the LLVM instruction builder used by the active generator. */
  llvm::IRBuilder<>& builder() { return *ctx.builder; }
  /** Provides the LLVM context that owns generated types and constants. */
  llvm::LLVMContext& llvmContext() { return ctx.getContext(); }

  /**
   * Restores the receiver — `this` and the owning class — on the way out.
   * Used where a class definition emits the bodies of its own methods.
   */
  struct ReceiverGuard {
    CodegenState& state;
    llvm::Value* savedThisPtr;
    std::shared_ptr<sun::semantic_analysis::ClassType> savedClass;

    /** Saves the active method receiver for restoration when the guard leaves scope. */
    explicit ReceiverGuard(CodegenState& s)
        : state(s),
          savedThisPtr(s.frame.thisPtr),
          savedClass(s.frame.currentClass) {}
    /** Restores the previous method receiver when the guard leaves scope. */
    ~ReceiverGuard() {
      state.frame.thisPtr = savedThisPtr;
      state.frame.currentClass = savedClass;
    }
    /** Saves the active method receiver for restoration when the guard leaves scope. */
    ReceiverGuard(const ReceiverGuard&) = delete;
    /** Disallows assignment so ownership and object identity cannot be duplicated. */
    ReceiverGuard& operator=(const ReceiverGuard&) = delete;
  };

  /**
   * Restores what the enclosing function's returns were allowed to do, so a
   * nested function or method body cannot leak its own contract outwards.
   */
  struct ReturnGuard {
    CodegenState& state;
    bool savedCanError;
    bool savedReturnsRef;
    llvm::Type* savedValueType;

    /** Saves the active function return state for restoration when the guard leaves scope. */
    explicit ReturnGuard(CodegenState& s)
        : state(s),
          savedCanError(s.frame.canError),
          savedReturnsRef(s.frame.returnsRef),
          savedValueType(s.frame.valueType) {}
    /** Restores the previous function return state when the guard leaves scope. */
    ~ReturnGuard() {
      state.frame.canError = savedCanError;
      state.frame.returnsRef = savedReturnsRef;
      state.frame.valueType = savedValueType;
    }
    /** Saves the active function return state for restoration when the guard leaves scope. */
    ReturnGuard(const ReturnGuard&) = delete;
    /** Disallows assignment so ownership and object identity cannot be duplicated. */
    ReturnGuard& operator=(const ReturnGuard&) = delete;
  };

  /**
   * Returns the builder to where it was, so emitting a nested function or a
   * synthesized helper does not strand the outer body's insertion point.
   * Restores nothing if the builder had no insertion point to begin with.
   */
  struct InsertPointGuard {
    CodegenState& state;
    llvm::BasicBlock* block;
    llvm::BasicBlock::iterator point;

    /** Saves the current LLVM insertion point for later restoration. */
    explicit InsertPointGuard(CodegenState& s)
        : state(s), block(s.ctx.builder->GetInsertBlock()) {
      if (block) point = s.ctx.builder->GetInsertPoint();
    }
    /** Restores the previous LLVM instruction insertion point. */
    ~InsertPointGuard() {
      if (block) state.ctx.builder->SetInsertPoint(block, point);
    }
    /** Saves the current LLVM insertion point for later restoration. */
    InsertPointGuard(const InsertPointGuard&) = delete;
    /** Disallows assignment so ownership and object identity cannot be duplicated. */
    InsertPointGuard& operator=(const InsertPointGuard&) = delete;
  };
};

}  // namespace sun::codegen
