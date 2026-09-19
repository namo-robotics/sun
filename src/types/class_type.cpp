/** Implements class relationships that require complete interface definitions.
 */
#include "types/class_type.h"

#include "types/interface_type.h"

/** Implements shared type descriptions and queries. */
namespace sun::types {
void ClassType::addImplementedInterface(const InterfaceType& interface) {
  if (!sameSession(interface))
    logAndThrowError(
        "Interface implementation belongs to another analysis session");
  if (!implementsInterface(interface))
    implementedInterfaces.push_back(interface.getDeclarationId());
}

bool ClassType::implementsInterface(const InterfaceType& interface) const {
  return sameSession(interface) &&
         std::find(implementedInterfaces.begin(), implementedInterfaces.end(),
                   interface.getDeclarationId()) != implementedInterfaces.end();
}

void ClassType::markStaticOnlyInterface(const InterfaceType& interface) {
  if (!sameSession(interface))
    logAndThrowError(
        "Interface implementation belongs to another analysis session");
  if (std::find(staticOnlyInterfaces.begin(), staticOnlyInterfaces.end(),
                interface.getDeclarationId()) == staticOnlyInterfaces.end())
    staticOnlyInterfaces.push_back(interface.getDeclarationId());
}

bool ClassType::convertibleToInterface(const InterfaceType& interface) const {
  return implementsInterface(interface) &&
         std::find(staticOnlyInterfaces.begin(), staticOnlyInterfaces.end(),
                   interface.getDeclarationId()) == staticOnlyInterfaces.end();
}

bool ClassType::isInterfaceConvertible(const TypePtr& from, const TypePtr& to) {
  if (!from || !to) return false;

  // Class -> Interface: the class is owned by the interface value. A borrow
  // never converts this way (it cannot become an owner), and neither does a
  // frame-carrying class (one that can hold a '<'_>' lambda): the interface
  // type would erase the frame binding.
  if (to->isInterface()) {
    if (!from->isClass() || typeIsFrameCarrying(from)) return false;
    auto* iface = static_cast<const InterfaceType*>(to.get());
    return static_cast<const ClassType*>(from.get())
        ->convertibleToInterface(*iface);
  }

  // Class -> ref Interface: the class is borrowed through the fat pointer
  if (to->isReference() && from->isClass()) {
    TypePtr target = unwrapRef(to);
    if (!target || !target->isInterface()) return false;
    auto* iface = static_cast<const InterfaceType*>(target.get());
    return static_cast<const ClassType*>(from.get())
        ->convertibleToInterface(*iface);
  }

  return false;
}

}  // namespace sun::types
