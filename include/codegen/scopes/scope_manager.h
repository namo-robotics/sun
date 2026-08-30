#pragma once

// scope_manager.h — The scope stack, and everything that gets dropped
//
// Two jobs that cannot sensibly be separated: remembering what each scope
// holds, and emitting the code that releases it. Sun never copies a compound
// value implicitly, so every class instance, payload enum and heap allocation
// has exactly one owner and exactly one release site — this is where those
// release sites are decided and written.
//
// A scope records its variables, the values it owns, and whether it marks a
// function entry. Leaving a scope drops what it still owns, innermost first,
// in reverse order of acquisition. Moving a value out marks it so its drop
// becomes a no-op.
//
// A move inside one arm of a branch is a move on that path only, so the paths
// beside it still own the value. Such a value gets a drop flag: a boolean in
// the frame saying whether this function still owns it, set where it became
// owned, cleared where the move happens, and read by the drop. The flag sits
// beside the value, never inside it, so class layout is untouched. Moves that
// are not inside a branch stay a compile-time decision costing nothing.

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/ValueHandle.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "codegen/codegen_state.h"
#include "semantic_analysis/types.h"
#include "support/position.h"

class CodegenVisitor;

using NamedValueMap = std::map<std::string, llvm::AllocaInst*>;

/**
 * A heap allocation the current scope owns and must free on the way out.
 */
struct OwnedAllocation {
  llvm::Value* ptrAlloca;    // Alloca storing the heap pointer
  std::string varName;       // Variable name (for debugging)
  bool moved;                // If true, ownership was transferred - don't free
  sun::TypePtr pointeeType;  // Type of the pointed-to object (for recursive
                             // field cleanup)
};

/**
 * A stack value the current scope must run drop code for: a class instance
 * (deinit plus field recursion) or a payload enum carrying owning payloads
 * (its synthesized drop function).
 */
struct ClassAllocation {
  // Address of the instance/storage. Usually an alloca; an owned lambda
  // capture is a slot inside the closure environment instead, so this is the
  // address rather than the alloca itself.
  llvm::Value* alloca;
  std::string varName;  // Variable name (for debugging)
  bool moved;           // If true, ownership transferred - don't drop
  sun::TypePtr type;    // Class or payload-enum type

  // Set once the value turns out to be moved inside a branch, so whether this
  // frame still owns it is only known at run time: an i1 slot the drop reads.
  // Null while moved-ness is the same on every path.
  llvm::AllocaInst* dropFlag = nullptr;

  // Where the value became owned, so a drop flag can be set there if one is
  // ever needed: an instruction to insert after, or the block to insert at the
  // top of. Weak, because it is only consulted if a branch move shows up.
  llvm::WeakTrackingVH ownedAt;

  // How many branch arms were open when the value became owned. A move nested
  // deeper than this happens on some paths only.
  unsigned branchDepth = 0;
};

/**
 * One scope's variables and the values it is responsible for releasing.
 */
struct CodegenScope {
  NamedValueMap variables;
  bool isFunctionBoundary = false;  // True for scopes marking function entry
  bool hasDebugScope = false;  // True when a DILexicalBlock was opened with it
  std::vector<OwnedAllocation> ownedAllocations;
  std::vector<ClassAllocation> classAllocations;
  // Names whose alloca holds a POINTER to the value rather than the value
  // itself (compound match-payload bindings borrow the payload slot in place)
  std::set<std::string> indirectBindings;
};

/**
 * Owns the scope stack for the function being emitted and writes the drop
 * code for everything in it.
 *
 * The stack itself is reachable through the container-shaped accessors
 * (`back()`, `size()`, `empty()`, `operator[]`) because callers legitimately
 * register a variable in the innermost scope or record how deep they are.
 * Everything else — finding a variable, taking ownership of a value, giving
 * it up, releasing what is left — goes through a named method.
 */
class ScopeManager {
 public:
  ScopeManager(CodegenState& state, CodegenVisitor& gen)
      : state_(state), gen_(gen), ctx(state.ctx) {}

  ScopeManager(const ScopeManager&) = delete;
  ScopeManager& operator=(const ScopeManager&) = delete;

  // ---------------------------------------------------------------
  // Branch arms
  // ---------------------------------------------------------------

  /**
   * Marks the code emitted while it is alive as one arm of a branch: a side of
   * an if/else, a match arm, a loop body, a try or catch block, the right side
   * of `and`/`or`. A value moved inside an arm is moved on that path only, so
   * it is given a drop flag and the paths beside it still drop it.
   */
  class BranchArm {
   public:
    explicit BranchArm(ScopeManager& scopes) : scopes_(scopes) {
      ++scopes_.branchDepth_;
    }
    ~BranchArm() { --scopes_.branchDepth_; }
    BranchArm(const BranchArm&) = delete;
    BranchArm& operator=(const BranchArm&) = delete;

   private:
    ScopeManager& scopes_;
  };

  // ---------------------------------------------------------------
  // The stack itself
  // ---------------------------------------------------------------

  bool empty() const { return scopes_.empty(); }
  size_t size() const { return scopes_.size(); }
  CodegenScope& back() { return scopes_.back(); }
  CodegenScope& operator[](size_t i) { return scopes_[i]; }

  // Open a scope that holds no source block of its own
  CodegenScope& push() {
    scopes_.emplace_back();
    return scopes_.back();
  }

  // Open a scope for a source block (if/else, loop, try/catch). Also opens a
  // DILexicalBlock so debuggers see block-accurate variable visibility (no-op
  // without -g); pop() closes it symmetrically.
  CodegenScope& push(const Position& loc);

  // Close the innermost scope, dropping whatever it still owns
  void pop();

  // Index of the innermost function-boundary scope. Falls back to the
  // innermost scope (old single-scope cleanup behavior) if none is marked,
  // so an unmarked context can never emit references into another function.
  size_t functionBoundaryDepth() const {
    for (size_t i = scopes_.size(); i-- > 0;) {
      if (scopes_[i].isFunctionBoundary) return i;
    }
    return scopes_.empty() ? 0 : scopes_.size() - 1;
  }

  // ---------------------------------------------------------------
  // Finding variables
  // ---------------------------------------------------------------

  /**
   * Finds a variable in the current scope chain.
   * Respects function boundaries - doesn't search past outer function scopes.
   * Variables from outer functions should be accessed via closures instead.
   */
  llvm::AllocaInst* findVariable(const std::string& name);

  // True if `name` resolves (in the current function) to an indirect binding
  // — its alloca holds the value's address, not the value
  bool isIndirectBinding(const std::string& name) const;

  // Storage address of a compound local: the alloca itself, or for an
  // indirect binding the pointer it holds
  llvm::Value* compoundStorageAddress(const std::string& name);

  // ---------------------------------------------------------------
  // Taking and giving up ownership
  // ---------------------------------------------------------------

  // Track a new owned allocation in the current scope
  void trackOwnedAllocation(llvm::Value* ptrAlloca, const std::string& name,
                            sun::TypePtr pointeeType = nullptr) {
    if (!scopes_.empty()) {
      scopes_.back().ownedAllocations.push_back(
          {ptrAlloca, name, false, std::move(pointeeType)});
    }
  }

  // Track a class or payload-enum allocation in the current scope for
  // automatic drop at scope exit. Enums are tracked only when they actually
  // need drop code. An alloca already tracked (e.g. a constructor temporary
  // later adopted by a variable) keeps its single entry — double-tracking
  // would double-drop.
  void trackClassAllocation(llvm::Value* alloca, const std::string& name,
                            sun::TypePtr type);

  // A block used as a value hands its result to the enclosing expression, and
  // ownership goes with it — the block's own scope must not drop it. Marks the
  // result moved out of the innermost scope and hands back the name it was
  // tracked under, so the caller can re-track it in the scope that owns it now
  // without losing the name its drop blocks are labelled with. Nothing means
  // that scope did not own it and there is nothing to hand on. Call this after
  // generating the body and before popping its scope.
  std::optional<std::string> releaseBlockResult(llvm::Value* result);

  // A call that hands back a compound by value hands back something the
  // caller now owns. `var x = f();` adopts the very same slot and
  // trackClassAllocation de-duplicates by alloca, and moving the result on
  // marks it deinited, so this only decides what happens when nobody takes
  // it: the temporary is dropped at the end of the scope that made it,
  // rather than leaked. Only a materialized return (an alloca) is a
  // temporary — a borrow handed back by a peek accessor is a pointer into
  // storage someone else owns, and typeNeedsDrop already says no to `ref T`.
  llvm::Value* trackCallTemporary(llvm::Value* result,
                                  const sun::TypePtr& resultType) {
    if (result && sun::typeNeedsDrop(resultType) &&
        llvm::isa<llvm::AllocaInst>(result)) {
      trackClassAllocation(result, "call.result", resultType);
    }
    return result;
  }

  // A by-value compound parameter arrives moved: the caller gave up its
  // ownership at the call, so this frame is the one that drops it. Passing it
  // on — into another call, a field, a container slot, a return — marks the
  // slot deinited, so this only decides what happens when the body keeps it
  // to the end. A `ref T` parameter is a borrow and answers false here.
  void trackOwnedParam(llvm::Value* alloca, const std::string& name,
                       const sun::TypePtr& type) {
    if (alloca && sun::typeNeedsDrop(type)) {
      trackClassAllocation(alloca, name, type);
    }
  }

  // Mark a class allocation as moved/deinited (don't auto-drop at scope exit).
  // Inside a branch arm the value is only moved on this path, so it also gets
  // a drop flag and its drop becomes a run-time decision.
  void markClassAllocationAsDeinited(llvm::Value* alloca);

  // Mark an owned allocation as moved (ownership transferred, don't free)
  void markAsMoved(const std::string& name);

  // True if any scope at or above `depth` holds a live (non-moved) owner —
  // i.e. unwinding past this point would need cleanup
  bool hasLiveOwners(size_t depth) const;

  // ---------------------------------------------------------------
  // Emitting drops
  // ---------------------------------------------------------------

  // Emit cleanup for every scope from the innermost down to the innermost
  // function boundary. Used by return paths and function ends.
  void emitScopeCleanup() { emitCleanupToDepth(functionBoundaryDepth()); }

  // Emit cleanup for all scopes from the innermost down to index `depth`
  // (inclusive), without popping any. Used by break/continue/throw paths that
  // jump out of several scopes at once.
  void emitCleanupToDepth(size_t depth);

  // Emit cleanup for a single scope's allocations (LIFO), without popping it
  void emitCleanupForScope(CodegenScope& scope);

  // Drop whatever value of `type` lives at `ptr`, in place: class deinit +
  // field recursion, or the enum drop function. No-op for other types.
  void emitDropInPlace(const sun::TypePtr& type, llvm::Value* ptr,
                       const std::string& name = "drop");

  // Call classType's deinit() on receiver if it defines one (declares the
  // external on demand).
  void emitDeinitCall(const sun::ClassType* classType, llvm::Value* receiver);

  // Emit deinit calls for class fields that have deinit methods. Recursively
  // deinits nested class fields; enum-typed fields with owning payloads are
  // dropped through their synthesized drop function.
  void emitFieldDeinit(llvm::Value* objectPtr, const sun::ClassType* classType,
                       const std::string& baseName);

  // Emit cleanup code for raw_ptr<T> fields in a class, recursively freeing
  // pointer fields before the containing object is freed.
  void emitFieldCleanup(llvm::Value* objectPtr, const sun::ClassType* classType,
                        const std::string& baseName,
                        llvm::FunctionCallee freeFunc);

  // Get or emit the synthesized drop function for a payload enum with owning
  // payloads: `void __sun_enum_drop$<Enum>(ptr storage)` switches on the tag,
  // drops each owning payload, then poisons the tag so a second drop is a
  // no-op. Returns nullptr when the enum needs no drop code.
  llvm::Function* getOrCreateEnumDropFunction(sun::EnumType& enumType);

  // Emit a drop of the payload-enum storage at `storagePtr` (no-op when the
  // enum needs no drop code)
  void emitEnumDrop(sun::EnumType& enumType, llvm::Value* storagePtr);

 private:
  // Give `alloc` a drop flag if it has none: an i1 slot that starts false in
  // the entry block and is set where the value became owned, so it is true
  // exactly when this frame still owns the value.
  void ensureDropFlag(ClassAllocation& alloc);

  // Drop `alloc` only if its drop flag says this frame still owns it, then
  // clear the flag.
  void emitFlaggedDrop(const ClassAllocation& alloc);

  CodegenState& state_;
  CodegenVisitor& gen_;
  CodegenContext& ctx;

  std::vector<CodegenScope> scopes_;

  // How many branch arms are currently open (see BranchArm)
  unsigned branchDepth_ = 0;
};
