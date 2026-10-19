#include "abi/semantic_requirements.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <deque>
#include <map>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <vector>

#include "support/error.h"

/** Describes the declarations and retained bodies interpreted by an importer. */
namespace sun::abi {
/** Implements the schema-specific semantic encoder without protobuf wire bytes. */
namespace {
/** Encodes a fixed-width integer in canonical little-endian order. */
std::string integer(uint64_t value) {
  std::string result;
  for (unsigned i = 0; i < 64; i += 8) result += static_cast<char>(value >> i);
  return result;
}
/** Frames arbitrary semantic bytes without ambiguity. */
std::string field(uint64_t tag, const std::string& bytes) {
  return integer(tag) + integer(bytes.size()) + bytes;
}
/** Excludes source diagnostics and metadata already represented elsewhere. */
bool omitted(const google::protobuf::FieldDescriptor& descriptor) {
  if (descriptor.name() == "source_file_id" || descriptor.name() == "doc" ||
      descriptor.name() == "source_text") return true;
  if (descriptor.message_type() &&
      descriptor.message_type()->full_name() == "sun.proto.ast.Position") return true;
  if (descriptor.containing_type()->full_name() != "sun.moon.ModuleMetadata") return false;
  const auto& name = descriptor.name();
  return name == "bundle_contract" || name == "source_path" || name == "source_hash" ||
         name == "version" || name == "content_hash" || name == "target_triple" ||
         name == "dependencies";
}
/** Declaration maps are sorted; field lists, statements and parameters stay ordered. */
bool unordered(const google::protobuf::FieldDescriptor& descriptor) {
  if (descriptor.containing_type()->full_name() != "sun.moon.ModuleMetadata") return false;
  const auto& name = descriptor.name();
  return name == "declarations" || name == "functions" || name == "classes" ||
         name == "interfaces" || name == "enums" || name == "globals";
}
/** Encodes known semantic fields and rejects unknown enum or syntax-node semantics. */
std::string encode(const google::protobuf::Message& message, unsigned depth) {
  if (depth > 128)
    sun::support::logAndThrowError("Unsupported retained metadata nesting depth");
  const auto* descriptor = message.GetDescriptor();
  const auto* reflection = message.GetReflection();
  if (descriptor->full_name() == "sun.proto.ast.ASTNode") {
    const auto* node = descriptor->FindOneofByName("node");
    if (!reflection->HasOneof(message, node))
      sun::support::logAndThrowError("Retained metadata contains an absent or unsupported syntax node");
  }
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  std::sort(fields.begin(), fields.end(), [](const auto* a, const auto* b) {
    return a->number() < b->number();
  });
  std::string result = field(0, descriptor->full_name());
  for (const auto* descriptor : fields) {
    if (omitted(*descriptor)) continue;
    const bool repeated = descriptor->is_repeated();
    const int count = repeated ? reflection->FieldSize(message, descriptor) : 1;
    std::vector<std::string> values;
    for (int i = 0; i < count; ++i) {
      /** Protobuf field categories select explicit canonical primitive encodings. */
      using Field = google::protobuf::FieldDescriptor;
      std::string value;
      switch (descriptor->cpp_type()) {
        case Field::CPPTYPE_INT32:
          value = integer(repeated ? reflection->GetRepeatedInt32(message, descriptor, i) : reflection->GetInt32(message, descriptor)); break;
        case Field::CPPTYPE_INT64:
          value = integer(repeated ? reflection->GetRepeatedInt64(message, descriptor, i) : reflection->GetInt64(message, descriptor)); break;
        case Field::CPPTYPE_UINT32:
          value = integer(repeated ? reflection->GetRepeatedUInt32(message, descriptor, i) : reflection->GetUInt32(message, descriptor)); break;
        case Field::CPPTYPE_UINT64:
          value = integer(repeated ? reflection->GetRepeatedUInt64(message, descriptor, i) : reflection->GetUInt64(message, descriptor)); break;
        case Field::CPPTYPE_BOOL:
          value = integer(repeated ? reflection->GetRepeatedBool(message, descriptor, i) : reflection->GetBool(message, descriptor)); break;
        case Field::CPPTYPE_ENUM: {
          const int number = repeated ? reflection->GetRepeatedEnumValue(message, descriptor, i) : reflection->GetEnumValue(message, descriptor);
          if (!descriptor->enum_type()->FindValueByNumber(number))
            sun::support::logAndThrowError("Unsupported retained metadata enum: " + descriptor->full_name());
          value = integer(number); break;
        }
        case Field::CPPTYPE_FLOAT:
          value = integer(std::bit_cast<uint32_t>(repeated ? reflection->GetRepeatedFloat(message, descriptor, i) : reflection->GetFloat(message, descriptor))); break;
        case Field::CPPTYPE_DOUBLE:
          value = integer(std::bit_cast<uint64_t>(repeated ? reflection->GetRepeatedDouble(message, descriptor, i) : reflection->GetDouble(message, descriptor))); break;
        case Field::CPPTYPE_STRING:
          value = repeated ? reflection->GetRepeatedString(message, descriptor, i) : reflection->GetString(message, descriptor); break;
        case Field::CPPTYPE_MESSAGE:
          value = encode(repeated ? reflection->GetRepeatedMessage(message, descriptor, i) : reflection->GetMessage(message, descriptor), depth + 1); break;
      }
      values.push_back(std::move(value));
    }
    if (unordered(*descriptor)) std::sort(values.begin(), values.end());
    for (const auto& value : values) result += field(descriptor->number(), value);
  }
  return result;
}
}  // namespace

std::string describeSemantics(const moon::ModuleMetadata& metadata) {
  return field(0, "sun.abi.metadata.v1") + encode(metadata, 0);
}
void validateDeclarationGraph(const std::vector<const moon::ModuleMetadata*>& modules) {
  std::map<std::string, const moon::DeclarationRecord*> records;
  for (const auto* module : modules) {
    for (const auto& record : module->declarations()) {
      if (record.key().empty() || !moon::DeclarationKind_IsValid(record.kind()))
        sun::support::logAndThrowError("Invalid declaration identity or kind");
      if (!records.emplace(record.key(), &record).second)
        sun::support::logAndThrowError("Duplicate declaration identity: " + record.key());
    }
  }
  std::map<std::string, size_t> pending;
  std::map<std::string, std::vector<std::string>> dependents;
  std::deque<std::string> ready;
  for (const auto& [key, record] : records) {
    auto& count = pending[key];
    for (const auto* reference : {&record->owner(), &record->module()}) {
      if (reference->empty()) continue;
      const auto found = records.find(*reference);
      if (found == records.end())
        sun::support::logAndThrowError("Missing declaration owner or module: " + *reference);
      if (reference == &record->module() && found->second->kind() != moon::MODULE)
        sun::support::logAndThrowError("Declaration module reference does not name a module");
      ++count;
      dependents[*reference].push_back(key);
    }
    if (!count) ready.push_back(key);
  }
  size_t completed = 0;
  while (!ready.empty()) {
    const auto key = std::move(ready.front());
    ready.pop_front();
    ++completed;
    for (const auto& dependent : dependents[key])
      if (--pending[dependent] == 0) ready.push_back(dependent);
  }
  if (completed != records.size())
    sun::support::logAndThrowError("Declaration ownership contains a cycle");
  for (const auto* module : modules)
    for (const auto& key : module->module_declarations()) {
      const auto found = records.find(key);
      if (found == records.end() || found->second->kind() != moon::MODULE)
        sun::support::logAndThrowError("Missing or invalid indexed module declaration: " + key);
    }
}
}  // namespace sun::abi
