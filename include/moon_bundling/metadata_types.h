#pragma once
#include <google/protobuf/message.h>

#include "semantic_analysis/semantic_context.h"
#include "types.pb.h"

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {
/** Convert a resolved type to an exportable annotation with exact nominal
 * identities. */
sun::proto::ast::TypeAnnotation exportType(
    const sun::semantic_analysis::TypePtr& type,
    const sun::semantic_analysis::DeclarationTable& declarations);
/** Bind serialized declarations in their definition context without
 * specializing templates. */
void bindMetadataTypes(google::protobuf::Message& message,
                       sun::semantic_analysis::SemanticContext& context);
/** Bind module expressions and using targets before exporting generic bodies.
 */
void bindMetadataModules(google::protobuf::Message& message,
                         sun::semantic_analysis::SemanticContext& context);
}  // namespace sun::moon_bundling
