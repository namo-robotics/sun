#pragma once

#include "semantic_analysis/qualified_name.h"
#include "types.pb.h"

namespace sun::serialization {
/** Serialize a qualified name without changing its scope or owner. */
inline ast::QualifiedName serializeQualifiedName(const QualifiedName& name) {
  ast::QualifiedName out;
  for (const auto& part : name.scopePath) out.add_scope_path(part);
  out.set_base_name(name.baseName);
  out.set_param_suffix(name.paramSuffix);
  for (const auto& part : name.modulePath) out.add_module_path(part);
  return out;
}

/** Restore a qualified name, including its original module owner. */
inline QualifiedName deserializeQualifiedName(const ast::QualifiedName& name) {
  QualifiedName out({name.scope_path().begin(), name.scope_path().end()},
                    name.base_name(),
                    {name.module_path().begin(), name.module_path().end()});
  out.paramSuffix = name.param_suffix();
  return out;
}
}  // namespace sun::serialization
