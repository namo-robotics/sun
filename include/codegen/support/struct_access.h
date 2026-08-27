#pragma once

// struct_access.h — Reading and writing class fields
//
// Where a field lives, how it is aligned, and the one rule that must never be
// forgotten: codegen of a class-valued expression yields the object's
// ADDRESS, so writing a class into a slot is a copy, not a store. Both take
// only a builder and a data layout, so they hold no codegen state.
//
// The alignment helpers are thin wrappers over sun::packed
// (semantic_analysis/packed_layout.h) that supply the module's DataLayout.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Alignment.h>

#include <string>

#include "ast.h"
#include "semantic_analysis/types.h"

namespace sun::codegen::layout {

/**
 * Address of one field of a class instance.
 */
llvm::Value* fieldPtr(llvm::IRBuilder<>& builder, sun::ClassType* classType,
                      llvm::Value* objectPtr, const sun::ClassField& field,
                      const std::string& name);

/**
 * Alignment for a field access, honouring packed layout. `owner` is the class
 * the field belongs to; a packed owner drops the alignment to 1. Pass nullptr
 * for a standalone slot such as a local variable.
 */
llvm::Align fieldAlign(const sun::ClassType* owner, llvm::Type* fieldTy,
                       const llvm::DataLayout& dl);

/**
 * Alignment for writing through an lvalue.
 */
llvm::Align lvalueAlign(const ExprAST& target, llvm::Type* slotTy,
                        const llvm::DataLayout& dl);

/**
 * Writes a value into a storage slot, copying the struct when the slot is a
 * class.
 *
 * Codegen of a class-valued expression yields the object's ADDRESS, not the
 * struct itself. Storing that address would write a pointer over the object's
 * leading bytes — and it fits silently, because a two-word class is exactly
 * pointer-sized, so the mistake surfaces as corrupted fields rather than as a
 * verifier error. Every site that writes a class into storage goes through
 * here so the copy cannot be forgotten again.
 */
void storeIntoSlot(llvm::IRBuilder<>& builder, const llvm::DataLayout& dl,
                   llvm::Value* dest, llvm::Value* value,
                   const sun::TypePtr& slotType,
                   const sun::ClassType* owner = nullptr);

}  // namespace sun::codegen::layout
