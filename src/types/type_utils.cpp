/** Walks type descriptions to compute cleanup and lifetime properties. */
#include "types/type_utils.h"

#include <unordered_set>

#include "types/class_type.h"
#include "types/enum_type.h"
#include "types/type_factory.h"

/** Implements shared type descriptions and queries. */
namespace sun::types {
/** Keeps recursive type walks private to this translation unit. */
namespace {
/**
 * True if dropping a value of this type must run cleanup code: classes with a
 * deinit method (directly, or transitively through class/enum-typed fields)
 * and payload enums with at least one payload that needs drop.
 */
bool typeNeedsDropImpl(const Type* type,
                       std::unordered_set<const Type*>& visited) {
  if (!type || !visited.insert(type).second) return false;
  if (type->isClass()) {
    auto* c = static_cast<const ClassType*>(type);
    if (c->getMethod("deinit")) return true;
    for (const auto& field : c->getFields()) {
      if (field.type && typeNeedsDropImpl(field.type.get(), visited)) {
        return true;
      }
    }
    return false;
  }
  if (type->isEnum()) {
    auto* e = static_cast<const EnumType*>(type);
    for (const auto& v : e->getVariants()) {
      for (const auto& pt : v.payloadTypes) {
        if (pt && typeNeedsDropImpl(pt.get(), visited)) return true;
      }
    }
    return false;
  }
  // A sized array owns its elements and drops each of them; an unsized array
  // is a view and owns nothing.
  if (type->isArray()) {
    auto* a = static_cast<const ArrayType*>(type);
    return !a->isUnsized() && a->getElementType() &&
           typeNeedsDropImpl(a->getElementType().get(), visited);
  }
  return false;
}

}  // namespace

/** Reports whether values of this type require cleanup when their lifetime
 * ends. */
bool typeNeedsDrop(const Type* type) {
  std::unordered_set<const Type*> visited;
  return typeNeedsDropImpl(type, visited);
}

/** Reports whether values of this type require cleanup when their lifetime
 * ends. */
bool typeNeedsDrop(const TypePtr& type) { return typeNeedsDrop(type.get()); }

/** Keeps recursive type walks private to this translation unit. */
namespace {
/**
 * True if a value of this type may carry a lambda environment that lives in
 * a stack frame: a '<'_>' lambda, or anything that can transitively hold
 * one — a class through its fields or generic type arguments (containers
 * hide elements behind raw storage, so the arguments must count), a payload
 * enum, an array. Such a value must not outlive the frame it was built in.
 * References are the sibling case, tracked by the borrow checker's
 * class-stores-refs walk.
 */
bool typeIsFrameCarryingImpl(const Type* type,
                             std::unordered_set<const Type*>& visited) {
  if (!type || !visited.insert(type).second) return false;
  if (type->isLambda()) {
    return static_cast<const LambdaType*>(type)->hasRefCaptures();
  }
  if (type->isClass()) {
    auto* c = static_cast<const ClassType*>(type);
    for (const auto& field : c->getFields()) {
      if (field.type && typeIsFrameCarryingImpl(field.type.get(), visited)) {
        return true;
      }
    }
    for (const auto& arg : c->getTypeArguments()) {
      if (arg && typeIsFrameCarryingImpl(arg.get(), visited)) return true;
    }
    return false;
  }
  if (type->isEnum()) {
    auto* e = static_cast<const EnumType*>(type);
    for (const auto& v : e->getVariants()) {
      for (const auto& pt : v.payloadTypes) {
        if (pt && typeIsFrameCarryingImpl(pt.get(), visited)) return true;
      }
    }
    for (const auto& arg : e->getGenericArgs()) {
      if (arg && typeIsFrameCarryingImpl(arg.get(), visited)) return true;
    }
    return false;
  }
  if (type->isArray()) {
    auto* a = static_cast<const ArrayType*>(type);
    return a->getElementType() &&
           typeIsFrameCarryingImpl(a->getElementType().get(), visited);
  }
  return false;
}

}  // namespace

/** Reports whether this type can retain storage tied to a stack frame. */
bool typeIsFrameCarrying(const Type* type) {
  std::unordered_set<const Type*> visited;
  return typeIsFrameCarryingImpl(type, visited);
}

/** Reports whether this type can retain storage tied to a stack frame. */
bool typeIsFrameCarrying(const TypePtr& type) {
  return typeIsFrameCarrying(type.get());
}

/**
 * A copy of the type with every lifetime NAME stripped, recursively.
 * Lifetime names are relative to one signature's lifetime list; a type that
 * crosses into another namespace - a generic type-parameter binding, whose
 * specialization is shared by every caller - must not carry them along.
 * The <'_> marker itself is identity and stays.
 */
TypePtr eraseLifetimeNames(const TypePtr& type) {
  if (!type) return type;
  if (auto* lt = dynamic_cast<const LambdaType*>(type.get())) {
    bool named = !lt->getLifetimeName().empty();
    std::vector<TypePtr> params;
    bool changed = named;
    for (const auto& param : lt->getParamTypes()) {
      auto stripped = eraseLifetimeNames(param);
      changed = changed || stripped != param;
      params.push_back(std::move(stripped));
    }
    auto ret = eraseLifetimeNames(lt->getReturnType());
    changed = changed || ret != lt->getReturnType();
    if (!changed) return type;
    auto result = Types::Lambda(ret, std::move(params), lt->canThrow(),
                                lt->requiresUnsafe());
    static_cast<LambdaType*>(result.get())
        ->setHasRefCaptures(lt->hasRefCaptures());
    return result;
  }
  if (auto* rt = dynamic_cast<const ReferenceType*>(type.get())) {
    if (rt->getLifetimeName().empty() && rt->getClassLifetimeArgs().empty()) {
      auto referent = eraseLifetimeNames(rt->getReferencedType());
      if (referent == rt->getReferencedType()) return type;
      return Types::Reference(referent, rt->isMutable());
    }
    return Types::Reference(eraseLifetimeNames(rt->getReferencedType()),
                            rt->isMutable());
  }
  return type;
}

}  // namespace sun::types
