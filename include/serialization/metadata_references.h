#pragma once
#include <google/protobuf/message.h>

#include <functional>

#include "ast.pb.h"
#include "semantic_analysis/types.h"
#include "serialization/qualified_name.h"

namespace sun::serialization {
/** Visit canonical names and identify requirements imposed by interface uses.
 */
inline void visitQualifiedNames(
    const google::protobuf::Message& message,
    const std::function<void(const QualifiedName&, std::optional<Type::Kind>)>&
        visit) {
  if (message.GetDescriptor() == ast::QualifiedName::descriptor()) {
    visit(deserializeQualifiedName(
              static_cast<const ast::QualifiedName&>(message)),
          std::nullopt);
    return;
  }
  const bool requiresInterface =
      message.GetDescriptor() == ast::ImplementedInterface::descriptor() ||
      message.GetDescriptor() == ast::TypeParameter::descriptor();
  const auto* reflection = message.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->name() == "module_qualified_name") continue;
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
      continue;
    if (requiresInterface && field->name() == "qualified_name") {
      visit(deserializeQualifiedName(static_cast<const ast::QualifiedName&>(
                reflection->GetMessage(message, field))),
            Type::Kind::Interface);
      continue;
    }
    if (field->is_repeated()) {
      for (int i = 0; i < reflection->FieldSize(message, field); ++i)
        visitQualifiedNames(reflection->GetRepeatedMessage(message, field, i),
                            visit);
    } else
      visitQualifiedNames(reflection->GetMessage(message, field), visit);
  }
}
}  // namespace sun::serialization
