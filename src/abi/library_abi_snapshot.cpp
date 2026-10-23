#include "abi/library_abi_snapshot.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <functional>
#include <set>

#include "abi/nominal_identity.h"
#include "abi/semantic_requirements.h"
#include "moon_bundling/moon.h"
#include "support/error.h"

/** Validates correspondence between revisions without changing code ownership.
 */
namespace sun::abi {
/** Keeps canonical encodings private to the revision protocol. */
namespace {
/** Frames arbitrary bytes without delimiter ambiguities. */
std::string frame(const std::string& value) {
  std::string result;
  const uint64_t size = value.size();
  for (unsigned shift = 0; shift < 64; shift += 8) {
    result += static_cast<char>(size >> shift);
  }
  return result + value;
}
/** Recognizes fields that contain declaration identities rather than literals.
 */
bool isReference(const google::protobuf::FieldDescriptor& field) {
  const auto& owner = field.containing_type()->full_name();
  return owner == "sun.proto.ast.DeclarationIdentity" ||
         field.name() == "declaration_key" ||
         field.name() == "module_declaration_key" ||
         (owner == "sun.moon.ModuleMetadata" &&
          field.name() == "module_declarations") ||
         (owner == "sun.moon.DeclarationRecord" &&
          (field.name() == "key" || field.name() == "owner" ||
           field.name() == "module"));
}
}  // namespace

void remapDeclarationReferences(
    google::protobuf::Message& message,
    const std::map<std::string, std::string>& replacements) {
  const auto* reflection = message.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    const bool repeated = field->is_repeated();
    const int count = repeated ? reflection->FieldSize(message, field) : 1;
    for (int i = 0; i < count; ++i) {
      if (field->cpp_type() ==
          google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        remapDeclarationReferences(
            *(repeated ? reflection->MutableRepeatedMessage(&message, field, i)
                       : reflection->MutableMessage(&message, field)),
            replacements);
      } else if (field->cpp_type() ==
                     google::protobuf::FieldDescriptor::CPPTYPE_STRING &&
                 isReference(*field)) {
        const auto value =
            repeated ? reflection->GetRepeatedString(message, field, i)
                     : reflection->GetString(message, field);
        if (const auto found = replacements.find(value);
            found != replacements.end()) {
          if (repeated) {
            reflection->SetRepeatedString(&message, field, i, found->second);
          } else {
            reflection->SetString(&message, field, found->second);
          }
        }
      }
    }
  }
}

std::string encodeLibraryAbiSnapshot(
    const proto::LibraryAbiSnapshot& revision) {
  std::set<std::string> declarations;
  for (const auto& item : revision.declarations()) {
    declarations.insert(frame(item.key()) + frame(item.identity()));
  }
  std::string result = frame(revision.library_id()) +
                       frame(revision.artifact_hash()) +
                       frame(revision.abi_hash());
  for (const auto& item : declarations) {
    result += frame(item);
  }
  return result;
}

proto::LibraryAbiSnapshot describeLibraryAbiSnapshot(
    const BundleContract& contract, const std::string& artifact,
    const std::vector<const moon::ModuleMetadata*>& modules) {
  validateLibraryId(contract.library_id());
  std::map<std::string, const moon::DeclarationRecord*> records;
  for (const auto* module : modules) {
    for (const auto& record : module->declarations()) {
      records.emplace(record.key(), &record);
    }
  }
  std::map<std::string, std::vector<std::string>> paths;
  std::set<std::string> active;
  /** Follows validated ownership, excluding the artifact's synthetic root. */
  const std::function<std::vector<std::string>(const std::string&)> path =
      [&](const std::string& key) -> std::vector<std::string> {
    if (key.empty()) {
      return {};
    }
    if (auto found = paths.find(key); found != paths.end()) {
      return found->second;
    }
    const auto found = records.find(key);
    if (found == records.end() || !active.insert(key).second) {
      sun::support::logAndThrowError("Invalid library revision ownership");
    }
    const auto& record = *found->second;
    auto result = path(record.owner());
    if (!(record.kind() == moon::MODULE && record.owner().empty() &&
          record.name() == "$" + artifact + "$")) {
      result.push_back(record.name());
    }
    active.erase(key);
    paths.emplace(key, result);
    return result;
  };
  std::map<std::string, std::string> identities;
  for (const auto& dependency : contract.dependency_abis()) {
    for (const auto& item : dependency.declarations()) {
      const auto [it, added] = identities.emplace(item.key(), item.identity());
      if (!added && it->second != item.identity()) {
        sun::support::logAndThrowError(
            "Conflicting dependency nominal identity");
      }
    }
  }
  proto::LibraryAbiSnapshot revision;
  revision.set_library_id(contract.library_id());
  revision.set_artifact_hash(artifact);
  std::set<std::string> unique;
  for (const auto& [key, record] : records) {
    const auto components = path(key);
    std::string identity;
    if (record->kind() == moon::CLASS || record->kind() == moon::INTERFACE ||
        record->kind() == moon::ENUM) {
      const auto kind = record->kind() == moon::CLASS ? NominalKind::Class
                        : record->kind() == moon::INTERFACE
                            ? NominalKind::Interface
                            : NominalKind::Enum;
      identity =
          NominalIdentity(contract.library_id(), kind, components).encoding();
    } else {
      identity =
          frame(contract.library_id()) + frame(std::to_string(record->kind()));
      for (const auto& component : components) {
        identity += frame(component);
      }
      identity = "sun.abi.path.v1:" + moon_bundling::computeSha256Hex(identity);
    }
    identities[key] = identity;
    // Only nominal types and module navigation can cross revision boundaries.
    // Function implementations and generated specializations retain exact keys.
    if (record->kind() == moon::CLASS || record->kind() == moon::INTERFACE ||
        record->kind() == moon::ENUM || record->kind() == moon::MODULE) {
      if (!unique.insert(identity).second) {
        sun::support::logAndThrowError("Ambiguous nominal library declaration");
      }
      auto* item = revision.add_declarations();
      item->set_key(key);
      item->set_identity(identity);
    }
  }
  // These requirements govern interpretation and lowering of the source ABI.
  // Each candidate's exact payload boundaries are independently validated.
  BundleContract requirements;
  requirements.set_encoding_schema(contract.encoding_schema());
  requirements.set_experimental(contract.experimental());
  *requirements.mutable_target() = contract.target();
  *requirements.mutable_requirements() = contract.requirements();
  // Preserve the original hash domain so renaming the snapshot keeps ABI hashes
  // stable.
  std::string encoded = frame("sun.abi.library-revision.v1") +
                        frame(encodeBundleContract(requirements));
  std::set<std::string> semantics;
  for (const auto* module : modules) {
    auto normalized = *module;
    // Implementation-local declarations have no place in the exported ABI.
    normalized.clear_declarations();
    for (auto& cls : *normalized.mutable_classes()) {
      cls.clear_compiled_specializations();
    }
    remapDeclarationReferences(normalized, identities);
    semantics.insert(describeSemantics(normalized));
  }
  for (const auto& item : semantics) {
    encoded += frame(item);
  }
  revision.set_abi_hash(moon_bundling::computeSha256Hex(encoded));
  return revision;
}
}  // namespace sun::abi
