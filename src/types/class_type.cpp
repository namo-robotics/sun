/** Implements class relationships that require complete interface definitions.
 */
#include "types/class_type.h"

#include "types/argument_compatibility.h"
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

const ClassMethod* ClassType::getMethodForArgs(
    const std::string& methodName, const std::vector<TypePtr>& argTypes) const {
  const ClassMethod* bestMatch = nullptr;
  for (const auto& method : methods) {
    if (method.name != methodName) continue;
    if (method.paramTypes.size() != argTypes.size()) continue;

    bool allMatch = true;
    bool allExact = true;
    for (size_t i = 0; i < argTypes.size(); ++i) {
      if (!argTypes[i] || !method.paramTypes[i]) {
        allMatch = false;
        break;
      }
      if (method.paramTypes[i]->equals(*argTypes[i])) continue;
      allExact = false;
      if (!argumentAccepts(argTypes[i], method.paramTypes[i], false)) {
        allMatch = false;
        break;
      }
    }
    if (!allMatch) continue;
    if (allExact) return &method;
    if (!bestMatch) bestMatch = &method;
  }
  return bestMatch;
}

}  // namespace sun::types
