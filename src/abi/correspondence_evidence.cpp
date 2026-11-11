/** Compares retained implementations without treating a producer hash as proof.
 */
#include "abi/correspondence_evidence.h"

#include <functional>

#include "abi/library_abi_snapshot.h"
#include "abi/semantic_requirements.h"
#include "support/error.h"

/** Checks correspondence between independently compiled declarations. */
namespace sun::abi {
/** Keeps proof traversal helpers private. */
namespace {
/** Provides reflection over retained proof inputs. */
using Message = google::protobuf::Message;
/** Describes one retained field without interpreting source spelling. */
using Field = google::protobuf::FieldDescriptor;
/** Names the retained syntax schema. */
namespace ast = sun::proto::ast;

/** Visits retained messages in deterministic field and element order. */
void walk(Message& message, const std::function<void(Message&)>& visit,
          unsigned depth = 0) {
  if (depth > 128) {
    sun::support::logAndThrowError(
        "Unsupported correspondence proof nesting depth");
  }
  visit(message);
  const auto* reflection = message.GetReflection();
  std::vector<const Field*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->cpp_type() != Field::CPPTYPE_MESSAGE) {
      continue;
    }
    if (field->is_repeated()) {
      for (int i = 0; i < reflection->FieldSize(message, field); ++i) {
        walk(*reflection->MutableRepeatedMessage(&message, field, i), visit,
             depth + 1);
      }
    } else {
      walk(*reflection->MutableMessage(&message, field), visit, depth + 1);
    }
  }
}

/** Returns a definition's exact source identity, including method prototypes.
 */
std::string definitionKey(const ast::ASTNode& node) {
  if (node.has_function_def()) {
    return node.function_def().proto().declaration_identity().key();
  }
  if (node.has_class_def()) {
    return node.class_def().declaration_identity().key();
  }
  if (node.has_enum_def()) {
    return node.enum_def().declaration_identity().key();
  }
  return node.declaration_identity().key();
}

/** Removes source bookkeeping while preserving executable operations. */
void clean(Message& message) {
  walk(message, [](Message& part) {
    if (part.GetDescriptor() == ast::ASTNode::descriptor()) {
      auto& node = static_cast<ast::ASTNode&>(part);
      node.clear_precompiled();
      node.clear_skip_codegen();
      node.clear_symbol_prefix();
    }
    if (part.GetDescriptor() == ast::ClassDef::descriptor()) {
      static_cast<ast::ClassDef&>(part).clear_compiled_specializations();
    }
  });
}

/** Collects selected executable dependencies rather than declarations of
 * locals. */
std::set<std::string> references(ast::ASTNode node) {
  std::set<std::string> result;
  walk(node, [&](Message& part) {
    const auto* reflection = part.GetReflection();
    for (const char* name : {"resolved_declaration_key", "declaration_key"}) {
      const auto* field = part.GetDescriptor()->FindFieldByName(name);
      if (field && field->cpp_type() == Field::CPPTYPE_STRING &&
          !field->is_repeated() && reflection->HasField(part, field)) {
        result.insert(reflection->GetString(part, field));
      }
    }
  });
  return result;
}

/** Finds operations that a plain-storage proof cannot safely ignore. */
std::string unsupportedOperation(ast::ASTNode node, bool plain) {
  std::string reason;
  walk(node, [&](Message& part) {
    if (plain && part.GetDescriptor() == ast::Prototype::descriptor() &&
        static_cast<const ast::Prototype&>(part).is_unsafe_method()) {
      reason = "plain-value operations require unsafe assumptions";
    }
    if (!reason.empty() || part.GetDescriptor() != ast::ASTNode::descriptor()) {
      return;
    }
    const auto& expression = static_cast<const ast::ASTNode&>(part);
    if (plain && expression.has_unsafe_block()) {
      reason = "plain-value operations contain unsafe assumptions";
    }
    if (expression.has_lambda_expr()) {
      reason = "closure environment correspondence is unsupported";
    }
    if (expression.has_call_expr()) {
      const auto& callee = expression.call_expr().callee();
      if (!expression.has_resolved_declaration_key() &&
          !callee.has_resolved_declaration_key()) {
        std::string name;
        if (callee.has_variable_reference()) {
          name = callee.variable_reference().name();
        } else if (callee.has_member_access()) {
          name = callee.member_access().member_name();
        }
        reason = "call target" +
                 (name.empty() ? std::string{} : " '" + name + "'") +
                 " is unresolved or indirect";
      }
    }
    if (expression.has_generic_call_expr() &&
        !expression.has_resolved_declaration_key()) {
      reason = "generic call target '" +
               expression.generic_call_expr().function_name() +
               "' is unresolved";
    }
  });
  return reason;
}
}  // namespace

void CorrespondenceEvidence::addDefinition(
    const sun::proto::ast::ASTNode& node) {
  if (!node.has_function_def() && !node.has_class_def() &&
      !node.has_enum_def()) {
    sun::support::logAndThrowError(
        "Unsupported correspondence proof definition");
  }
  if (node.has_function_def() && !node.function_def().is_c_extern() &&
      !node.function_def().body_present()) {
    sun::support::logAndThrowError(
        "Correspondence proof is missing a method or function body");
  }
  auto validated = node;
  walk(validated, [](Message& part) {
    if (!part.GetReflection()->GetUnknownFields(part).empty()) {
      sun::support::logAndThrowError(
          "Unknown correspondence proof input fields");
    }
  });
  const auto key = definitionKey(node);
  if (key.empty()) {
    sun::support::logAndThrowError(
        "Correspondence proof definition has no identity");
  }
  auto [it, inserted] = definitions_.emplace(key, node);
  if (!inserted && describeCorrespondenceInputs(it->second) !=
                       describeCorrespondenceInputs(node)) {
    sun::support::logAndThrowError(
        "Conflicting correspondence proof definitions");
  }
  if (node.has_class_def()) {
    for (const auto& method : node.class_def().methods()) {
      ast::ASTNode function;
      *function.mutable_function_def() = method.function();
      addDefinition(function);
    }
  }
}

void CorrespondenceEvidence::add(const moon::ModuleMetadata& metadata,
                                 const std::string& libraryIdentity) {
  comparisons_.clear();
  for (const auto& declaration : metadata.declarations()) {
    declarations_.emplace(declaration.key(), declaration);
    libraryIdentities_.emplace(declaration.key(), libraryIdentity);
  }
  for (const auto& global : metadata.globals()) {
    globals_.emplace(global.declaration_identity().key(), global);
  }
  if (!metadata.has_correspondence_proof()) {
    return;
  }
  const auto& proof = metadata.correspondence_proof();
  for (const auto& node : proof.definitions()) {
    if (proof.version() != 1) {
      unsupported_.insert(definitionKey(node));
    } else {
      addDefinition(node);
    }
  }
}

std::string CorrespondenceEvidence::correspondenceName(
    const std::string& key) const {
  std::vector<std::string> path;
  std::set<std::string> visited;
  auto current = key;
  while (!current.empty()) {
    auto found = declarations_.find(current);
    if (found == declarations_.end() || !visited.insert(current).second) {
      return key;
    }
    const auto& record = found->second;
    if (!(record.owner().empty() && record.kind() == moon::MODULE &&
          record.name().starts_with("$"))) {
      path.push_back(std::to_string(record.kind()) + ":" + record.name());
    }
    current = record.owner();
  }
  const auto library = libraryIdentities_.find(key);
  if (library == libraryIdentities_.end() || library->second.empty()) {
    return key;
  }
  std::string result =
      std::to_string(library->second.size()) + ":" + library->second;
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    result += std::to_string(it->size()) + ":" + *it;
  }
  return result;
}

CorrespondenceResult CorrespondenceEvidence::compare(const std::string& source,
                                                     const std::string& target,
                                                     bool plain) const {
  const auto key = std::make_tuple(source, target, plain);
  if (const auto cached = comparisons_.find(key);
      cached != comparisons_.end()) {
    return cached->second;
  }
  std::set<std::pair<std::string, std::string>> active;
  auto result = compareImpl(source, target, plain, active);
  if (result.proven && plain) {
    // Plain implementations may differ. Check dependencies introduced by either
    // side, including ones absent from the other revision's implementation.
    result = compareImpl(target, source, plain, active);
  }
  // Publish only the completed root result, never a provisional cycle result.
  comparisons_.emplace(key, result);
  return result;
}

CorrespondenceResult CorrespondenceEvidence::compareImpl(
    const std::string& source, const std::string& target, bool plain,
    std::set<std::pair<std::string, std::string>>& active) const {
  if (source.empty() || target.empty()) {
    return {
        false,
        "missing correspondence proof identity; rebuild the producing bundles"};
  }
  if (source == target) {
    return {true};
  }
  if (unsupported_.contains(source) || unsupported_.contains(target)) {
    return {false, "unsupported correspondence proof version"};
  }
  const auto from = definitions_.find(source);
  const auto to = definitions_.find(target);
  if (from == definitions_.end() || to == definitions_.end()) {
    return {
        false,
        "missing implementation proof records; rebuild the producing bundles"};
  }
  const auto name = correspondenceName(source);
  if (name != correspondenceName(target)) {
    return {false, "selected declarations do not correspond"};
  }
  auto left = from->second;
  auto right = to->second;
  const auto unsupported = unsupportedOperation(left, plain);
  const auto otherUnsupported = unsupportedOperation(right, plain);
  if (!unsupported.empty() || !otherUnsupported.empty()) {
    return {false, unsupported.empty() ? otherUnsupported : unsupported};
  }
  if (left.has_function_def() && (left.function_def().is_c_extern() ||
                                  right.function_def().is_c_extern())) {
    return {false, "native runtime provider compatibility is not established"};
  }
  const auto dependencies = references(left);
  const auto otherDependencies = references(right);
  std::map<std::string, std::string> replacements;
  for (const auto& [key, record] : declarations_) {
    replacements.emplace(key, correspondenceName(key));
  }
  clean(left);
  clean(right);
  remapDeclarationReferences(left, replacements);
  remapDeclarationReferences(right, replacements);
  if (!plain && describeCorrespondenceInputs(left) !=
                    describeCorrespondenceInputs(right)) {
    return {false, "resolved implementations differ"};
  }
  // Compare every node before admitting a back edge. An active cycle is only
  // a deferred obligation; no child result is retained as a cached proof.
  const auto pair = std::make_pair(source, target);
  if (!active.insert(pair).second) {
    return {true};
  }
  for (const auto& dependency : dependencies) {
    const auto record = declarations_.find(dependency);
    if (record == declarations_.end()) {
      active.erase(pair);
      return {false, "unresolved implementation dependency", {dependency}};
    }
    const auto kind = record->second.kind();
    if (kind != moon::FUNCTION && kind != moon::CLASS &&
        kind != moon::VARIABLE && kind != moon::ENUM) {
      continue;
    }
    if (kind == moon::VARIABLE && !globals_.contains(dependency)) {
      continue;
    }
    std::vector<std::string> matches;
    for (const auto& other : otherDependencies) {
      if (correspondenceName(dependency) == correspondenceName(other)) {
        matches.push_back(other);
      }
    }
    if (matches.size() != 1) {
      active.erase(pair);
      return {false,
              "missing or ambiguous dependency correspondence",
              {record->second.name()}};
    }
    if (kind == moon::VARIABLE && dependency != matches.front()) {
      active.erase(pair);
      return {false,
              "revision-local global state is not interchangeable",
              {record->second.name()}};
    }
    auto result = compareImpl(dependency, matches.front(), plain, active);
    if (!result.proven) {
      active.erase(pair);
      return result.within(record->second.name());
    }
  }
  active.erase(pair);
  return {true};
}
}  // namespace sun::abi
