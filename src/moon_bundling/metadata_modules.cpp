#include <set>

#include "ast.pb.h"
#include "moon_bundling/metadata_types.h"
#include "serialization/qualified_name.h"

namespace sun {
namespace {
using Names = std::set<std::string>;

std::string moduleSpelling(const ast::ASTNode& node) {
  if (node.has_variable_reference()) return node.variable_reference().name();
  if (node.has_qualified_name()) {
    const auto& parts = node.qualified_name().parts();
    return QualifiedName::joinPath({parts.begin(), parts.end()});
  }
  if (node.has_member_access() &&
      node.member_access().type_arguments().empty()) {
    auto parent = moduleSpelling(node.member_access().object());
    if (!parent.empty())
      return parent + "." + node.member_access().member_name();
  }
  return "";
}

void addLocal(const ast::ASTNode& node, Names& locals) {
  if (node.has_variable_creation())
    locals.insert(node.variable_creation().name());
  if (node.has_reference_creation())
    locals.insert(node.reference_creation().name());
  if (node.has_declare_type() && node.declare_type().has_alias_name())
    locals.insert(node.declare_type().alias_name());
}

// The traversal follows lexical scopes without instantiating generic types.
void bindModules(google::protobuf::Message& message, SemanticContext& ctx,
                 Names locals) {
  const auto* desc = message.GetDescriptor();
  const auto* reflection = message.GetReflection();
  auto source = desc->FindFieldByName("source_file_id");
  SemanticContext::SourceFileGuard file(
      ctx, source ? reflection->GetUInt64(message, source) : 0);

  if (desc == ast::TypeAnnotation::descriptor() ||
      desc == ast::QualifiedName::descriptor())
    return;

  if (desc == ast::ASTNode::descriptor()) {
    auto& node = static_cast<ast::ASTNode&>(message);
    if (node.has_module_qualified_name()) return;
    if (node.has_using_stmt()) {
      auto& use = *node.mutable_using_stmt();
      auto path = QualifiedName::joinPath(
          {use.namespace_path().begin(), use.namespace_path().end()});
      SemanticScopeBase* module = nullptr;
      if (!use.is_module_import()) {
        auto full = path.empty() ? use.target() : path + "." + use.target();
        module = ctx.lookupModuleScope(full);
        if (module) use.set_is_module_import(true);
      }
      if (!module) module = ctx.lookupModuleScope(path);
      if (module) {
        *node.mutable_module_qualified_name() =
            serialization::serializeQualifiedName(
                static_cast<const ModuleScope&>(*module).qualifiedName);
        auto target = use.is_module_import() ? "*" : use.target();
        ctx.addUsingImport(UsingImport(
            static_cast<const ModuleScope&>(*module).qualifiedName.lookupName(),
            target));
        ctx.addImportBinding(target == "*"
                                 ? ImportBinding::wildcard(module)
                                 : ImportBinding(target, module, target));
      }
      return;
    }
    auto path = moduleSpelling(node);
    auto first = path.substr(0, path.find('.'));
    if (!path.empty() && !locals.contains(first) &&
        !ctx.lookupVariable(first) && !ctx.lookupEnum(first) &&
        ctx.getAllFunctions(first).empty()) {
      if (auto* module = ctx.lookupModuleScope(path)) {
        *node.mutable_module_qualified_name() =
            serialization::serializeQualifiedName(
                static_cast<const ModuleScope&>(*module).qualifiedName);
        return;
      }
    }
  }

  if (desc == ast::BlockExpr::descriptor()) {
    auto& block = static_cast<ast::BlockExpr&>(message);
    SemanticContext::ScopeSwitchGuard scope(ctx, ctx.scope());
    ctx.enterScope();
    for (auto& node : *block.mutable_body()) {
      bindModules(node, ctx, locals);
      addLocal(node, locals);
    }
    return;
  }
  if (desc == ast::ForExpr::descriptor()) {
    auto& loop = static_cast<ast::ForExpr&>(message);
    SemanticContext::ScopeSwitchGuard scope(ctx, ctx.scope());
    ctx.enterScope();
    if (loop.has_init()) {
      bindModules(*loop.mutable_init(), ctx, locals);
      addLocal(loop.init(), locals);
    }
    if (loop.has_condition())
      bindModules(*loop.mutable_condition(), ctx, locals);
    if (loop.has_increment())
      bindModules(*loop.mutable_increment(), ctx, locals);
    bindModules(*loop.mutable_body(), ctx, locals);
    return;
  }
  if (desc == ast::ForInExpr::descriptor()) {
    auto& loop = static_cast<ast::ForInExpr&>(message);
    bindModules(*loop.mutable_iterable(), ctx, locals);
    locals.insert(loop.loop_var());
    bindModules(*loop.mutable_body(), ctx, locals);
    return;
  }
  if (desc == ast::MatchArm::descriptor()) {
    auto& arm = static_cast<ast::MatchArm&>(message);
    if (arm.has_pattern()) bindModules(*arm.mutable_pattern(), ctx, locals);
    for (const auto& binding : arm.bindings())
      if (!binding.is_wildcard()) locals.insert(binding.name());
    bindModules(*arm.mutable_body(), ctx, locals);
    return;
  }
  if (desc == ast::CatchClause::descriptor()) {
    auto& clause = static_cast<ast::CatchClause&>(message);
    locals.insert(clause.binding_name());
    bindModules(*clause.mutable_body(), ctx, locals);
    return;
  }

  // Template parameters, function arguments, captures, and class members can
  // shadow a module name even before a generic body has been analyzed.
  if (const auto* field = desc->FindFieldByName("type_params")) {
    for (int i = 0; i < reflection->FieldSize(message, field); ++i) {
      const auto& param = static_cast<const ast::TypeParameter&>(
          reflection->GetRepeatedMessage(message, field, i));
      locals.insert(param.name());
    }
  }
  if (const auto* field = desc->FindFieldByName("proto")) {
    const auto& proto = static_cast<const ast::Prototype&>(
        reflection->GetMessage(message, field));
    for (const auto& param : proto.type_params()) locals.insert(param.name());
    for (const auto& arg : proto.args()) locals.insert(arg.name());
    for (const auto& name : proto.ref_captures()) locals.insert(name);
    for (const auto& name : proto.owned_captures()) locals.insert(name);
    if (proto.has_variadic_param_name())
      locals.insert(proto.variadic_param_name());
  }
  if (desc == ast::ClassDef::descriptor()) {
    const auto& cls = static_cast<const ast::ClassDef&>(message);
    for (const auto& field : cls.fields()) locals.insert(field.name());
    for (const auto& method : cls.methods())
      locals.insert(method.function().proto().name());
  }

  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->cpp_type() !=
            google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE ||
        field->name() == "module_qualified_name")
      continue;
    if (field->is_repeated()) {
      for (int i = 0; i < reflection->FieldSize(message, field); ++i)
        bindModules(*reflection->MutableRepeatedMessage(&message, field, i),
                    ctx, locals);
    } else {
      bindModules(*reflection->MutableMessage(&message, field), ctx, locals);
    }
  }
}
}  // namespace

void bindMetadataModules(google::protobuf::Message& message,
                         SemanticContext& context) {
  SemanticContext::ScopeSwitchGuard scope(context, context.scope());
  context.enterScope();
  bindModules(message, context, {});
}
}  // namespace sun
