#pragma once
#include <google/protobuf/message.h>

#include "semantic_analysis/semantic_context.h"
#include "types.pb.h"

namespace sun {
/** Convert a resolved type to an exportable annotation with exact nominal
 * identities. */
ast::TypeAnnotation exportType(const TypePtr& type);
/** Bind serialized declarations in their definition context without
 * specializing templates. */
void bindMetadataTypes(google::protobuf::Message& message,
                       SemanticContext& context);
/** Bind module expressions and using targets before exporting generic bodies.
 */
void bindMetadataModules(google::protobuf::Message& message,
                         SemanticContext& context);
}  // namespace sun
