#include "moon_bundling/metadata_types.h"

#include <set>

#include "ast.pb.h"
#include "semantic_analysis/type_analysis/type_traits.h"

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {
namespace pbc = sun::proto::ast;

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/** Serializes a named type with its declaration identity and generic arguments.
 */
void nominal(pbc::TypeAnnotation& out, const sun::types::NominalType& type,
             const std::string& display,
             const std::vector<sun::types::TypePtr>& args,
             const sun::semantic_analysis::DeclarationTable& declarations) {
  out.set_base_name(display);
  if (display != "IError")
    out.set_declaration_key(
        sun::semantic_analysis::DeclarationId::forExport(
            type.sourceDeclaration(declarations), declarations)
            .encoding());
  for (const auto& arg : args)
    *out.add_type_arguments() = exportType(arg, declarations);
}

/**
 * A template's parameters are names, even when no specialization was requested.
 */
void parameters(const google::protobuf::Message& message,
                std::set<std::string>& names) {
  const auto* desc = message.GetDescriptor();
  const auto* reflection = message.GetReflection();
  if (const auto* field = desc->FindFieldByName("type_params")) {
    for (int i = 0; i < reflection->FieldSize(message, field); ++i) {
      const auto& parameter = reflection->GetRepeatedMessage(message, field, i);
      names.insert(parameter.GetReflection()->GetString(
          parameter, parameter.GetDescriptor()->FindFieldByName("name")));
    }
  }
  if (const auto* field = desc->FindFieldByName("proto"))
    parameters(reflection->GetMessage(message, field), names);
}

/** Binds serialized type references to declarations in the current analysis
 * context. */
void bindDeclarationTypes(google::protobuf::Message& message,
                          sun::semantic_analysis::SemanticContext& ctx,
                          std::set<std::string> names, bool inBody = false) {
  parameters(message, names);
  if (message.GetDescriptor() == pbc::TypeAnnotation::descriptor()) {
    auto& annotation = static_cast<pbc::TypeAnnotation&>(message);
    const auto& name = annotation.base_name();
    if (!annotation.has_declaration_key() && !names.contains(name) &&
        !sun::types::Types::fromString(name) &&
        !sun::semantic_analysis::type_analysis::isTypeTrait(name) &&
        name != "IError" && name != "_return_type_of" && name != "_params_of" &&
        name != "ref" && name != "raw_ptr" && name != "static_ptr" &&
        name != "array" && name != "fn" && name != "lambda") {
      sun::semantic_analysis::DeclarationId ref;
      if (auto* generic = ctx.lookupGenericClass(name))
        ref = generic->AST->getDeclarationId();
      else if (auto* generic = ctx.lookupGenericInterface(name))
        ref = generic->AST->getDeclarationId();
      else if (auto* generic = ctx.lookupGenericEnum(name))
        ref = generic->AST->getDeclarationId();
      else if (auto type = ctx.findTypeAlias(name)) {
        auto expanded = exportType(type, ctx.results().declarations);
        expanded.set_can_error(annotation.can_error() || expanded.can_error());
        if (!annotation.lifetime_arguments().empty())
          *expanded.mutable_lifetime_arguments() =
              annotation.lifetime_arguments();
        annotation = std::move(expanded);
      } else if (auto type = ctx.lookupClass(name))
        ref = type->sourceDeclaration(ctx.results().declarations);
      else if (auto type = ctx.lookupInterface(name))
        ref = type->sourceDeclaration(ctx.results().declarations);
      else if (auto type = ctx.lookupEnum(name))
        ref = type->sourceDeclaration(ctx.results().declarations);
      else if (!inBody)
        sun::support::logAndThrowError(
            "Cannot bind exported type '" + name + "'", ctx.currentLocation());
      if (ref)
        annotation.set_declaration_key(
            sun::semantic_analysis::DeclarationId::forExport(
                ref, ctx.results().declarations)
                .encoding());
    }
  } else if (message.GetDescriptor() ==
             pbc::ImplementedInterface::descriptor()) {
    auto& impl = static_cast<pbc::ImplementedInterface&>(message);
    pbc::TypeAnnotation annotation;
    annotation.set_base_name(impl.name());
    bindDeclarationTypes(annotation, ctx, names);
    if (annotation.has_declaration_key())
      impl.set_declaration_key(annotation.declaration_key());
  } else if (message.GetDescriptor() == pbc::TypeParameter::descriptor()) {
    auto& param = static_cast<pbc::TypeParameter&>(message);
    if (param.has_constraint() &&
        !sun::semantic_analysis::type_analysis::isTypeTrait(
            param.constraint())) {
      pbc::TypeAnnotation annotation;
      annotation.set_base_name(param.constraint());
      bindDeclarationTypes(annotation, ctx, names);
      if (annotation.has_declaration_key())
        param.set_declaration_key(annotation.declaration_key());
    }
  }
  const auto* reflection = message.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
      continue;
    bool body = inBody || field->name() == "body";
    if (field->is_repeated()) {
      for (int i = 0; i < reflection->FieldSize(message, field); ++i)
        bindDeclarationTypes(
            *reflection->MutableRepeatedMessage(&message, field, i), ctx, names,
            body);
    } else
      bindDeclarationTypes(*reflection->MutableMessage(&message, field), ctx,
                           names, body);
  }
}
}  // namespace

/** Serializes a semantic type with its portable declaration identity. */
pbc::TypeAnnotation exportType(
    const sun::types::TypePtr& type,
    const sun::semantic_analysis::DeclarationTable& declarations) {
  pbc::TypeAnnotation out;
  if (!type) sun::support::logAndThrowError("Cannot export an unresolved type");
  if (auto* value =
          sun::codegen::support::tryGetType<sun::types::ReferenceType>(type)) {
    out.set_base_name("ref");
    out.set_const_ref(!value->isMutable());
    out.set_lifetime_name(value->getLifetimeName());
    *out.mutable_element_type() =
        exportType(value->getReferencedType(), declarations);
    for (const auto& lifetime : value->getClassLifetimeArgs())
      out.mutable_element_type()->add_lifetime_arguments(lifetime);
  } else if (auto* value =
                 sun::codegen::support::tryGetType<sun::types::RawPointerType>(
                     type)) {
    out.set_base_name("raw_ptr");
    *out.mutable_element_type() =
        exportType(value->getPointeeType(), declarations);
  } else if (auto* value = sun::codegen::support::tryGetType<
                 sun::types::StaticPointerType>(type)) {
    out.set_base_name("static_ptr");
    *out.mutable_element_type() =
        exportType(value->getPointeeType(), declarations);
  } else if (auto* value =
                 sun::codegen::support::tryGetType<sun::types::ArrayType>(
                     type)) {
    out.set_base_name("array");
    *out.mutable_element_type() =
        exportType(value->getElementType(), declarations);
    for (auto dim : value->getDimensions())
      out.add_array_dimensions()->set_size(dim);
  } else if (auto* value =
                 sun::codegen::support::tryGetType<sun::types::FunctionType>(
                     type)) {
    out.set_base_name("fn");
    out.set_can_error(value->canThrow());
    out.set_requires_unsafe(value->requiresUnsafe());
    *out.mutable_return_type() =
        exportType(value->getReturnType(), declarations);
    for (const auto& arg : value->getParamTypes())
      *out.add_param_types() = exportType(arg, declarations);
  } else if (auto* value =
                 sun::codegen::support::tryGetType<sun::types::LambdaType>(
                     type)) {
    out.set_base_name("lambda");
    out.set_can_error(value->canThrow());
    out.set_requires_unsafe(value->requiresUnsafe());
    out.set_ref_env(value->hasRefCaptures());
    out.set_lifetime_name(value->getLifetimeName());
    *out.mutable_return_type() =
        exportType(value->getReturnType(), declarations);
    for (const auto& arg : value->getParamTypes())
      *out.add_param_types() = exportType(arg, declarations);
  } else if (auto* value =
                 sun::codegen::support::tryGetType<sun::types::ClassType>(
                     type)) {
    nominal(out, *value, value->toDisplayString(), value->getTypeArguments(),
            declarations);
  } else if (auto* value =
                 sun::codegen::support::tryGetType<sun::types::InterfaceType>(
                     type)) {
    nominal(out, *value, value->toDisplayString(), value->getTypeArguments(),
            declarations);
  } else if (auto* value =
                 sun::codegen::support::tryGetType<sun::types::EnumType>(
                     type)) {
    nominal(out, *value, value->getDisplayName(), value->getGenericArgs(),
            declarations);
  } else if (auto* value =
                 sun::codegen::support::tryGetType<sun::types::ErrorUnionType>(
                     type)) {
    out = exportType(value->getValueType(), declarations);
    out.set_can_error(true);
  } else
    out.set_base_name(type->toString());
  return out;
}

/** Resolves serialized type references in the active semantic context. */
void bindMetadataTypes(google::protobuf::Message& message,
                       sun::semantic_analysis::SemanticContext& context) {
  bindDeclarationTypes(message, context, {});
}
}  // namespace sun::moon_bundling
