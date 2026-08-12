// packed_layout.h — Rules for packed classes (`packed_class X { ... }`)
//
// A packed class is lowered to an LLVM packed struct: no padding between
// fields, struct alignment 1. That single layout change has consequences the
// rest of the compiler has to respect, and they are collected here so the
// definition of "packed" lives in one place:
//
//   - Fields land at arbitrary offsets, so every access must be emitted at
//     align 1. IRBuilder otherwise tags accesses with the field type's ABI
//     alignment, a false claim the optimizer is entitled to exploit.
//   - Packing is inherited down an access chain: a field of a non-packed class
//     is still misaligned if reached through a packed one (`p.inner.x`).
//   - A packed field's address cannot be handed out as a `ref`, for the same
//     reason C++ refuses to bind a reference to a packed member.

#pragma once

#include <string>

#include "ast/member_access_ast.h"
#include "types.h"

namespace sun::packed {

// The class a member access reads through, seeing past ref/raw_ptr/static_ptr.
// Returns nullptr when the object is not class-shaped.
inline const ClassType* accessedClass(const ExprAST& object) {
  auto objectType = unwrapRef(object.getResolvedType());
  if (objectType && objectType->isRawPointer()) {
    objectType =
        static_cast<RawPointerType*>(objectType.get())->getPointeeType();
  } else if (objectType && objectType->isStaticPointer()) {
    objectType =
        static_cast<StaticPointerType*>(objectType.get())->getPointeeType();
  }
  if (objectType && objectType->isClass()) {
    return static_cast<const ClassType*>(objectType.get());
  }
  return nullptr;
}

// True when `expr` names a field reached through a packed class at any depth.
// Packing removes padding at that level, so everything nested beneath it
// inherits an arbitrary offset. Reports the offending class via `ownerName`.
inline bool isFieldAccess(const ExprAST& expr,
                          std::string* ownerName = nullptr) {
  const ExprAST* cur = &expr;
  while (cur && cur->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& access = static_cast<const MemberAccessAST&>(*cur);
    const ExprAST* object = access.getObject();
    if (!object) break;

    if (const ClassType* owner = accessedClass(*object)) {
      if (owner->isPacked()) {
        if (ownerName) *ownerName = owner->getDisplayName();
        return true;
      }
    }
    cur = object;
  }
  return false;
}

// Alignment for accessing a field of `owner`.
inline llvm::Align fieldAlign(const ClassType* owner, llvm::Type* fieldTy,
                              const llvm::DataLayout& DL) {
  if (owner && owner->isPacked()) return llvm::Align(1);
  return DL.getABITypeAlign(fieldTy);
}

// Alignment for an assignable expression, honouring the whole access chain.
inline llvm::Align lvalueAlign(const ExprAST& target, llvm::Type* slotTy,
                               const llvm::DataLayout& DL) {
  if (isFieldAccess(target)) return llvm::Align(1);
  return DL.getABITypeAlign(slotTy);
}

// Why a field type is rejected inside a packed class, or empty if it is fine.
// Multi-word fat pointers have interior pointers that would land unaligned for
// no benefit, and an unpacked nested class would reintroduce exactly the
// interior padding the user asked to remove.
inline std::string rejectFieldType(const TypePtr& fieldType) {
  if (!fieldType) return {};
  if (fieldType->isArray()) {
    return "has array type. Arrays are fat pointers and cannot be packed. Use "
           "raw_ptr<T> instead.";
  }
  if (fieldType->isInterface()) {
    return "has interface type. Interface values are fat pointers and cannot "
           "be packed.";
  }
  if (fieldType->isClass()) {
    auto* nested = static_cast<const ClassType*>(fieldType.get());
    if (!nested->isPacked()) {
      return "has non-packed class type '" + nested->getDisplayName() +
             "'. Its interior padding would remain. Declare '" +
             nested->getDisplayName() + "' as a packed_class.";
    }
  }
  return {};
}

inline std::string fieldPhrase(const std::string& ownerName) {
  return "field of packed class '" + ownerName + "'";
}

// Shared explanation for the ways a packed field's address can escape.
// `what` completes "Cannot <what> - packed fields ...".
inline std::string borrowRejection(const std::string& what,
                                   const char* remedy) {
  return "Cannot " + what +
         " - packed fields have no padding and so are not aligned enough to "
         "be borrowed. " +
         remedy;
}

}  // namespace sun::packed
