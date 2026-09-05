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
  std::shared_ptr<sun::ClassType> currentClass = nullptr;

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
  std::shared_ptr<sun::TypeRegistry> typeRegistry;

  // sun::Type -> llvm::Type conversion, with its own cache
  LLVMTypeResolver typeResolver;

  // DWARF metadata emission; no-op unless -g
  sun::DebugInfoBuilder debugInfo;

  // Where the emitter currently is
  FunctionFrame frame;

  CodegenState(CodegenContext& ctx, std::shared_ptr<sun::TypeRegistry> registry)
      : ctx(ctx),
        module(ctx.mainModule.get()),
        typeRegistry(std::move(registry)),
        typeResolver(ctx.getContext(), &ctx.mainModule->getDataLayout()),
        debugInfo(ctx.mainModule.get(), ctx.debugInfoEnabled()) {}

  CodegenState(const CodegenState&) = delete;
  CodegenState& operator=(const CodegenState&) = delete;

  llvm::IRBuilder<>& builder() { return *ctx.builder; }
  llvm::LLVMContext& llvmContext() { return ctx.getContext(); }

  /**
   * Restores the receiver — `this` and the owning class — on the way out.
   * Used where a class definition emits the bodies of its own methods.
   */
  struct ReceiverGuard {
    CodegenState& state;
    llvm::Value* savedThisPtr;
    std::shared_ptr<sun::ClassType> savedClass;

    explicit ReceiverGuard(CodegenState& s)
        : state(s),
          savedThisPtr(s.frame.thisPtr),
          savedClass(s.frame.currentClass) {}
    ~ReceiverGuard() {
      state.frame.thisPtr = savedThisPtr;
      state.frame.currentClass = savedClass;
    }
    ReceiverGuard(const ReceiverGuard&) = delete;
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

    explicit ReturnGuard(CodegenState& s)
        : state(s),
          savedCanError(s.frame.canError),
          savedReturnsRef(s.frame.returnsRef),
          savedValueType(s.frame.valueType) {}
    ~ReturnGuard() {
      state.frame.canError = savedCanError;
      state.frame.returnsRef = savedReturnsRef;
      state.frame.valueType = savedValueType;
    }
    ReturnGuard(const ReturnGuard&) = delete;
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

    explicit InsertPointGuard(CodegenState& s)
        : state(s), block(s.ctx.builder->GetInsertBlock()) {
      if (block) point = s.ctx.builder->GetInsertPoint();
    }
    ~InsertPointGuard() {
      if (block) state.ctx.builder->SetInsertPoint(block, point);
    }
    InsertPointGuard(const InsertPointGuard&) = delete;
    InsertPointGuard& operator=(const InsertPointGuard&) = delete;
  };
};
