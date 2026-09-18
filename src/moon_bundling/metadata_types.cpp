#include "moon_bundling/metadata_types.h"

#include <set>

#include "ast.pb.h"
#include "semantic_analysis/type_traits.h"

namespace sun {
namespace {

void nominal(ast::TypeAnnotation& out, const NominalType& type,
             const std::string& display, const std::vector<TypePtr>& args,
             const DeclarationTable& declarations) {
  out.set_base_name(display);
  if (display != "IError")
    out.set_declaration_key(
        PortableDeclarationKey::fromDeclaration(
            type.sourceDeclaration(declarations), declarations)
            .encoding());
  for (const auto& arg : args)
    *out.add_type_arguments() = exportType(arg, declarations);
}

// A template's parameters are names, even when no specialization was requested.
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

void bindDeclarationTypes(google::protobuf::Message& message,
                          SemanticContext& ctx, std::set<std::string> names,
                          bool inBody = false) {
  parameters(message, names);
  if (message.GetDescriptor() == ast::TypeAnnotation::descriptor()) {
    auto& annotation = static_cast<ast::TypeAnnotation&>(message);
    const auto& name = annotation.base_name();
    if (!annotation.has_declaration_key() && !names.contains(name) &&
        !Types::fromString(name) && !isTypeTrait(name) && name != "IError" &&
        name != "_return_type_of" && name != "_params_of" && name != "ref" &&
        name != "raw_ptr" && name != "static_ptr" && name != "array" &&
        name != "fn" && name != "lambda") {
      DeclarationId ref;
      if (auto* generic = ctx.lookupGenericClass(name))
        ref = generic->AST->getDeclarationId();
      else if (auto* generic = ctx.lookupGenericInterface(name))
        ref = generic->AST->getDeclarationId();
      else if (auto* generic = ctx.lookupGenericEnum(name))
        ref = generic->AST->getDeclarationId();
      else if (auto type = ctx.findTypeAlias(name)) {
        auto expanded = exportType(type, ctx.types()->declarations);
        expanded.set_can_error(annotation.can_error() || expanded.can_error());
        if (!annotation.lifetime_arguments().empty())
          *expanded.mutable_lifetime_arguments() =
              annotation.lifetime_arguments();
        annotation = std::move(expanded);
      } else if (auto type = ctx.lookupClass(name))
        ref = type->sourceDeclaration(ctx.types()->declarations);
      else if (auto type = ctx.lookupInterface(name))
        ref = type->sourceDeclaration(ctx.types()->declarations);
      else if (auto type = ctx.lookupEnum(name))
        ref = type->sourceDeclaration(ctx.types()->declarations);
      else if (!inBody)
        logAndThrowError("Cannot bind exported type '" + name + "'",
                         ctx.currentLocation());
      if (ref)
        annotation.set_declaration_key(PortableDeclarationKey::fromDeclaration(
                                           ref, ctx.types()->declarations)
                                           .encoding());
    }
  } else if (message.GetDescriptor() ==
             ast::ImplementedInterface::descriptor()) {
    auto& impl = static_cast<ast::ImplementedInterface&>(message);
    ast::TypeAnnotation annotation;
    annotation.set_base_name(impl.name());
    bindDeclarationTypes(annotation, ctx, names);
    if (annotation.has_declaration_key())
      impl.set_declaration_key(annotation.declaration_key());
  } else if (message.GetDescriptor() == ast::TypeParameter::descriptor()) {
    auto& param = static_cast<ast::TypeParameter&>(message);
    if (param.has_constraint() && !isTypeTrait(param.constraint())) {
      ast::TypeAnnotation annotation;
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

ast::TypeAnnotation exportType(const TypePtr& type,
                               const DeclarationTable& declarations) {
  ast::TypeAnnotation out;
  if (!type) logAndThrowError("Cannot export an unresolved type");
  if (auto* value = tryGetType<ReferenceType>(type)) {
    out.set_base_name("ref");
    out.set_const_ref(!value->isMutable());
    out.set_lifetime_name(value->getLifetimeName());
    *out.mutable_element_type() =
        exportType(value->getReferencedType(), declarations);
    for (const auto& lifetime : value->getClassLifetimeArgs())
      out.mutable_element_type()->add_lifetime_arguments(lifetime);
  } else if (auto* value = tryGetType<RawPointerType>(type)) {
    out.set_base_name("raw_ptr");
    *out.mutable_element_type() =
        exportType(value->getPointeeType(), declarations);
  } else if (auto* value = tryGetType<StaticPointerType>(type)) {
    out.set_base_name("static_ptr");
    *out.mutable_element_type() =
        exportType(value->getPointeeType(), declarations);
  } else if (auto* value = tryGetType<ArrayType>(type)) {
    out.set_base_name("array");
    *out.mutable_element_type() =
        exportType(value->getElementType(), declarations);
    for (auto dim : value->getDimensions()) out.add_array_dimensions(dim);
  } else if (auto* value = tryGetType<FunctionType>(type)) {
    out.set_base_name("fn");
    out.set_can_error(value->canThrow());
    out.set_requires_unsafe(value->requiresUnsafe());
    *out.mutable_return_type() =
        exportType(value->getReturnType(), declarations);
    for (const auto& arg : value->getParamTypes())
      *out.add_param_types() = exportType(arg, declarations);
  } else if (auto* value = tryGetType<LambdaType>(type)) {
    out.set_base_name("lambda");
    out.set_can_error(value->canThrow());
    out.set_requires_unsafe(value->requiresUnsafe());
    out.set_ref_env(value->hasRefCaptures());
    out.set_lifetime_name(value->getLifetimeName());
    *out.mutable_return_type() =
        exportType(value->getReturnType(), declarations);
    for (const auto& arg : value->getParamTypes())
      *out.add_param_types() = exportType(arg, declarations);
  } else if (auto* value = tryGetType<ClassType>(type)) {
    nominal(out, *value, value->toDisplayString(), value->getTypeArguments(),
            declarations);
  } else if (auto* value = tryGetType<InterfaceType>(type)) {
    nominal(out, *value, value->toDisplayString(), value->getTypeArguments(),
            declarations);
  } else if (auto* value = tryGetType<EnumType>(type)) {
    nominal(out, *value, value->getDisplayName(), value->getGenericArgs(),
            declarations);
  } else if (auto* value = tryGetType<ErrorUnionType>(type)) {
    out = exportType(value->getValueType(), declarations);
    out.set_can_error(true);
  } else
    out.set_base_name(type->toString());
  return out;
}

void bindMetadataTypes(google::protobuf::Message& message,
                       SemanticContext& context) {
  bindDeclarationTypes(message, context, {});
}
}  // namespace sun
