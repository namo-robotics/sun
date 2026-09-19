#include <set>

#include "ast.pb.h"
#include "moon_bundling/metadata_types.h"


using sun::semantic_analysis::ModuleScope;
using sun::semantic_analysis::SemanticContext;

namespace sun::moon_bundling {

using ScopeSwitchGuard = sun::semantic_analysis::SemanticContext::ScopeSwitchGuard;
namespace pbc = sun::proto::ast;

namespace {
using Names = std::set<std::string>;

std::string moduleSpelling(const pbc::ASTNode& node) {
  if (node.has_variable_reference()) return node.variable_reference().name();
  if (node.has_qualified_name()) {
    const auto& parts = node.qualified_name().parts();
    return sun::semantic_analysis::QualifiedName::joinPath(
        {parts.begin(), parts.end()});
  }
  if (node.has_member_access() &&
      node.member_access().type_arguments().empty()) {
    auto parent = moduleSpelling(node.member_access().object());
    if (!parent.empty())
      return parent + "." + node.member_access().member_name();
  }
  return "";
}

void addLocal(const pbc::ASTNode& node, Names& locals) {
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
  sun::semantic_analysis::SemanticContext::SourceFileGuard file(
      ctx, source ? reflection->GetUInt64(message, source) : 0);

  if (desc == pbc::TypeAnnotation::descriptor()) return;

  if (desc == pbc::ASTNode::descriptor()) {
    auto& node = static_cast<pbc::ASTNode&>(message);
    if (node.has_module_declaration_key()) return;
    if (node.has_using_stmt()) {
      auto& use = *node.mutable_using_stmt();
      auto path = sun::semantic_analysis::QualifiedName::joinPath(
          {use.namespace_path().begin(), use.namespace_path().end()});
      sun::semantic_analysis::SemanticScopeBase* module = nullptr;
      if (!use.is_module_import()) {
        auto full = path.empty() ? use.target() : path + "." + use.target();
        module = ctx.lookupModuleScope(full);
        if (module) use.set_is_module_import(true);
      }
      if (!module) module = ctx.lookupModuleScope(path);
      if (module) {
        node.set_module_declaration_key(
            sun::semantic_analysis::PortableDeclarationKey::fromDeclaration(
                static_cast<const ModuleScope&>(*module).declarationId,
                ctx.types()->declarations)
                .encoding());
        auto target = use.is_module_import() ? "*" : use.target();
        ctx.addUsingImport(sun::semantic_analysis::UsingImport(
            static_cast<const ModuleScope&>(*module).qualifiedName.lookupName(),
            target));
        ctx.addImportBinding(
            target == "*"
                ? sun::semantic_analysis::ImportBinding::wildcard(module)
                : sun::semantic_analysis::ImportBinding(target, module,
                                                        target));
      }
      return;
    }
    auto path = moduleSpelling(node);
    auto first = path.substr(0, path.find('.'));
    if (!path.empty() && !locals.contains(first) &&
        !ctx.currentScope().lookupVariable(first) && !ctx.lookupEnum(first) &&
        ctx.getAllFunctions(first).empty()) {
      if (auto* module = ctx.lookupModuleScope(path)) {
        node.set_module_declaration_key(
            sun::semantic_analysis::PortableDeclarationKey::fromDeclaration(
                static_cast<const ModuleScope&>(*module).declarationId,
                ctx.types()->declarations)
                .encoding());
        return;
      }
    }
  }

  if (desc == pbc::BlockExpr::descriptor()) {
    auto& block = static_cast<pbc::BlockExpr&>(message);
    ScopeSwitchGuard scope(ctx, ctx.scope());
    ctx.enterScope();
    for (auto& node : *block.mutable_body()) {
      bindModules(node, ctx, locals);
      addLocal(node, locals);
    }
    return;
  }
  if (desc == pbc::ForExpr::descriptor()) {
    auto& loop = static_cast<pbc::ForExpr&>(message);
    ScopeSwitchGuard scope(ctx, ctx.scope());
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
  if (desc == pbc::ForInExpr::descriptor()) {
    auto& loop = static_cast<pbc::ForInExpr&>(message);
    bindModules(*loop.mutable_iterable(), ctx, locals);
    locals.insert(loop.loop_var());
    bindModules(*loop.mutable_body(), ctx, locals);
    return;
  }
  if (desc == pbc::MatchArm::descriptor()) {
    auto& arm = static_cast<pbc::MatchArm&>(message);
    if (arm.has_pattern()) bindModules(*arm.mutable_pattern(), ctx, locals);
    for (const auto& binding : arm.bindings())
      if (!binding.is_wildcard()) locals.insert(binding.name());
    bindModules(*arm.mutable_body(), ctx, locals);
    return;
  }
  if (desc == pbc::CatchClause::descriptor()) {
    auto& clause = static_cast<pbc::CatchClause&>(message);
    locals.insert(clause.binding_name());
    bindModules(*clause.mutable_body(), ctx, locals);
    return;
  }

  // Template parameters, function arguments, captures, and class members can
  // shadow a module name even before a generic body has been analyzed.
  if (const auto* field = desc->FindFieldByName("type_params")) {
    for (int i = 0; i < reflection->FieldSize(message, field); ++i) {
      const auto& param = static_cast<const pbc::TypeParameter&>(
          reflection->GetRepeatedMessage(message, field, i));
      locals.insert(param.name());
    }
  }
  if (const auto* field = desc->FindFieldByName("proto")) {
    const auto& proto = static_cast<const pbc::Prototype&>(
        reflection->GetMessage(message, field));
    for (const auto& param : proto.type_params()) locals.insert(param.name());
    for (const auto& arg : proto.args()) locals.insert(arg.name());
    for (const auto& name : proto.ref_captures()) locals.insert(name);
    for (const auto& name : proto.owned_captures()) locals.insert(name);
    if (proto.has_variadic_param_name())
      locals.insert(proto.variadic_param_name());
  }
  if (desc == pbc::ClassDef::descriptor()) {
    const auto& cls = static_cast<const pbc::ClassDef&>(message);
    for (const auto& field : cls.fields()) locals.insert(field.name());
    for (const auto& method : cls.methods())
      locals.insert(method.function().proto().name());
  }

  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
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
  ScopeSwitchGuard scope(context, context.scope());
  context.enterScope();
  bindModules(message, context, {});
}
}  // namespace sun::moon_bundling
