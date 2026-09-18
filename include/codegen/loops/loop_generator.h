#pragma once

namespace sun::codegen {
class CodegenVisitor;
}
namespace sun::codegen::functions {
class FunctionRegistry;
}
namespace sun::codegen::scopes {
class ScopeManager;
}

// loop_generator.h — for, for-in and while, and the jumps out of them
//
// Every loop pushes a context recording where `continue` and `break` go and
// how deep the loop body's scope sits. A jump out of a loop is not just a
// branch: it leaves scopes that still own values, so it drops them down to
// the loop body's depth before branching.
//
// `for … in` is not a builtin. It calls iter() if the iterable has one, then
// next(ref Container) until it answers None, against the IIterator/IIterable
// interfaces in stdlib/iterator.sun.

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Value.h>

#include <type_traits>
#include <vector>

#include "ast.h"
#include "codegen/codegen_state.h"

namespace sun::codegen::loops {
using sun::ast::ExprAST;

/**
 * Where break and continue go, and how much has to be dropped to get there.
 */
struct LoopContext {
  llvm::BasicBlock* continueBlock;  // Block to jump to for 'continue'
  llvm::BasicBlock* breakBlock;     // Block to jump to for 'break'
  size_t cleanupDepth;  // Scope index of the loop body; break/continue emit
                        // cleanup for scopes at or above this depth
};

/**
 * Emits the three loop forms and the two ways out of them.
 */
class LoopGenerator {
 public:
  LoopGenerator(sun::codegen::CodegenState& state,
                sun::codegen::CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        typeResolver(state.typeResolver) {}

  LoopGenerator(const LoopGenerator&) = delete;
  LoopGenerator& operator=(const LoopGenerator&) = delete;

  llvm::Value* codegen(const sun::ast::ForExprAST& expr);
  llvm::Value* codegen(const sun::ast::ForInExprAST& expr);
  llvm::Value* codegen(const sun::ast::WhileExprAST& expr);
  llvm::Value* codegen(const sun::ast::BreakAST& expr);
  llvm::Value* codegen(const sun::ast::ContinueAST& expr);

 private:
  sun::codegen::CodegenState& state_;
  sun::codegen::CodegenVisitor& gen_;
  sun::codegen::CodegenContext& ctx;

  // Stack of loop contexts for break/continue handling
  std::vector<LoopContext> loopStack;

  // Aliases into the shared state, so the emission code reads the same way
  // the rest of codegen does
  sun::codegen::LLVMTypeResolver& typeResolver;

  // What a loop borrows from the rest of codegen: emitting its parts, finding
  // the iterator protocol's methods, and declaring the loop variable.
  llvm::Value* codegen(const ExprAST& expr);
  llvm::Value* codegen(const sun::ast::BlockExprAST& block);

  // A node kind with its own overload must not silently bind to the
  // ExprAST forwarder above: that path attaches an expression debug location,
  // so a block routed through it changes DWARF output. Make it a compile
  // error instead. Add an overload here when a new kind is needed.
  template <typename T>
    requires(!std::is_same_v<T, ExprAST> &&
             !std::is_same_v<T, sun::ast::BlockExprAST> &&
             std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  sun::codegen::scopes::ScopeManager& scopes();
  sun::codegen::functions::FunctionRegistry& functions();
  llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                           llvm::StringRef varName,
                                           llvm::Type* type);
  void debugDeclareLocal(llvm::AllocaInst* alloca, const std::string& name,
                         const sun::semantic_analysis::TypePtr& type,
                         const sun::support::Position& loc);
  llvm::Value* materializeMethodClosure(llvm::Value* fnPtr,
                                        llvm::Value* receiverPtr,
                                        llvm::StringRef name);
};

}  // namespace sun::codegen::loops
