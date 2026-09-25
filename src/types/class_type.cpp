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
  if (interface.getParent()) addImplementedInterface(*interface.getParent());
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
  if (!from || !to || !to->isReference()) return false;
  if (from->isReference() &&
      !refMutabilityConvertible(*static_cast<const ReferenceType*>(from.get()),
                                *static_cast<const ReferenceType*>(to.get())))
    return false;
  auto source = unwrapRef(from);
  auto target = unwrapRef(to);
  if (!source || !target || !target->isInterface()) return false;
  auto* interface = static_cast<const InterfaceType*>(target.get());
  if (source->isInterface())
    return static_cast<const InterfaceType*>(source.get())
        ->extendsInterface(*interface);
  if (source->isClass())
    return static_cast<const ClassType*>(source.get())
        ->convertibleToInterface(*interface);

  return false;
}

}  // namespace sun::types
