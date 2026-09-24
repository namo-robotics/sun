#pragma once
#include <google/protobuf/message.h>

#include <functional>

#include "ast.pb.h"
#include "types/types.h"

/** Converts syntax trees to and from the compiler protobuf representation. */
namespace sun::serialization {
namespace pbc = sun::proto::ast;

/** Visit portable nominal references, retaining names only for diagnostics. */
inline void visitDeclarationKeys(
    const google::protobuf::Message& message,
    const std::function<void(
        const sun::semantic_analysis::PortableDeclarationKey&,
        std::optional<sun::types::Type::Kind>, const std::string&)>& visit) {
  const auto* descriptor = message.GetDescriptor();
  if (descriptor == pbc::CompiledSpecialization::descriptor()) return;
  const auto* reflection = message.GetReflection();
  if (const auto* field = descriptor->FindFieldByName("declaration_key")) {
    if (reflection->HasField(message, field)) {
      const bool interface =
          descriptor == pbc::ImplementedInterface::descriptor() ||
          descriptor == pbc::TypeParameter::descriptor();
      const auto* name = descriptor->FindFieldByName(
          descriptor == pbc::TypeAnnotation::descriptor()  ? "base_name"
          : descriptor == pbc::TypeParameter::descriptor() ? "constraint"
                                                           : "name");
      visit(sun::semantic_analysis::PortableDeclarationKey::fromString(
                reflection->GetString(message, field)),
            interface ? std::optional<sun::types::Type::Kind>(
                            sun::types::Type::Kind::Interface)
                      : std::nullopt,
            name ? reflection->GetString(message, name) : "");
    }
  }
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
      continue;
    if (field->is_repeated()) {
      for (int i = 0; i < reflection->FieldSize(message, field); ++i)
        visitDeclarationKeys(reflection->GetRepeatedMessage(message, field, i),
                             visit);
    } else
      visitDeclarationKeys(reflection->GetMessage(message, field), visit);
  }
}
}  // namespace sun::serialization
