#include "abi/semantic_requirements.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "semantic_analysis/analysis_results.h"
#include "semantic_analysis/semantic_context.h"
#include "support/error.h"

/** Describes the declarations and retained bodies interpreted by an importer.
 */
namespace sun::abi {
/** Implements the schema-specific semantic encoder without protobuf wire bytes.
 */
namespace {
/** Encodes a fixed-width integer in canonical little-endian order. */
std::string integer(uint64_t value) {
  std::string result;
  for (unsigned i = 0; i < 64; i += 8) {
    result += static_cast<char>(value >> i);
  }
  return result;
}
/** Frames arbitrary semantic bytes without ambiguity. */
std::string field(uint64_t tag, const std::string& bytes) {
  return integer(tag) + integer(bytes.size()) + bytes;
}
/** Excludes source diagnostics and metadata already represented elsewhere. */
bool omitted(const google::protobuf::FieldDescriptor& descriptor) {
  if (descriptor.name() == "source_file_id" || descriptor.name() == "doc" ||
      descriptor.name() == "source_text") {
    return true;
  }
  if (descriptor.message_type() &&
      descriptor.message_type()->full_name() == "sun.proto.ast.Position") {
    return true;
  }
  if (descriptor.containing_type()->full_name() != "sun.moon.ModuleMetadata") {
    return false;
  }
  const auto& name = descriptor.name();
  return name == "bundle_contract" || name == "source_path" ||
         name == "source_hash" || name == "version" || name == "content_hash" ||
         name == "target_triple" || name == "dependencies";
}
/** Declaration maps are sorted; field lists, statements and parameters stay
 * ordered. */
bool unordered(const google::protobuf::FieldDescriptor& descriptor) {
  if (descriptor.containing_type()->full_name() != "sun.moon.ModuleMetadata") {
    return false;
  }
  const auto& name = descriptor.name();
  return name == "declarations" || name == "functions" || name == "classes" ||
         name == "interfaces" || name == "enums" || name == "globals";
}
/** Encodes known semantic fields and rejects unknown enum or syntax-node
 * semantics. */
std::string encode(const google::protobuf::Message& message, unsigned depth) {
  if (depth > 128) {
    sun::support::logAndThrowError(
        "Unsupported retained metadata nesting depth");
  }
  const auto* descriptor = message.GetDescriptor();
  const auto* reflection = message.GetReflection();
  if (descriptor->full_name() == "sun.proto.ast.ASTNode") {
    const auto* node = descriptor->FindOneofByName("node");
    if (!reflection->HasOneof(message, node)) {
      sun::support::logAndThrowError(
          "Retained metadata contains an absent or unsupported syntax node");
    }
  }
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  std::sort(fields.begin(), fields.end(), [](const auto* a, const auto* b) {
    return a->number() < b->number();
  });
  std::string result = field(0, descriptor->full_name());
  for (const auto* descriptor : fields) {
    if (omitted(*descriptor)) {
      continue;
    }
    const bool repeated = descriptor->is_repeated();
    const int count = repeated ? reflection->FieldSize(message, descriptor) : 1;
    std::vector<std::string> values;
    for (int i = 0; i < count; ++i) {
      /** Protobuf field categories select explicit canonical primitive
       * encodings. */
      using Field = google::protobuf::FieldDescriptor;
      std::string value;
      switch (descriptor->cpp_type()) {
        case Field::CPPTYPE_INT32:
          value = integer(
              repeated ? reflection->GetRepeatedInt32(message, descriptor, i)
                       : reflection->GetInt32(message, descriptor));
          break;
        case Field::CPPTYPE_INT64:
          value = integer(
              repeated ? reflection->GetRepeatedInt64(message, descriptor, i)
                       : reflection->GetInt64(message, descriptor));
          break;
        case Field::CPPTYPE_UINT32:
          value = integer(
              repeated ? reflection->GetRepeatedUInt32(message, descriptor, i)
                       : reflection->GetUInt32(message, descriptor));
          break;
        case Field::CPPTYPE_UINT64:
          value = integer(
              repeated ? reflection->GetRepeatedUInt64(message, descriptor, i)
                       : reflection->GetUInt64(message, descriptor));
          break;
        case Field::CPPTYPE_BOOL:
          value = integer(
              repeated ? reflection->GetRepeatedBool(message, descriptor, i)
                       : reflection->GetBool(message, descriptor));
          break;
        case Field::CPPTYPE_ENUM: {
          const int number =
              repeated
                  ? reflection->GetRepeatedEnumValue(message, descriptor, i)
                  : reflection->GetEnumValue(message, descriptor);
          if (!descriptor->enum_type()->FindValueByNumber(number)) {
            sun::support::logAndThrowError(
                "Unsupported retained metadata enum: " +
                descriptor->full_name());
          }
          value = integer(number);
          break;
        }
        case Field::CPPTYPE_FLOAT:
          value = integer(std::bit_cast<uint32_t>(
              repeated ? reflection->GetRepeatedFloat(message, descriptor, i)
                       : reflection->GetFloat(message, descriptor)));
          break;
        case Field::CPPTYPE_DOUBLE:
          value = integer(std::bit_cast<uint64_t>(
              repeated ? reflection->GetRepeatedDouble(message, descriptor, i)
                       : reflection->GetDouble(message, descriptor)));
          break;
        case Field::CPPTYPE_STRING:
          value = repeated
                      ? reflection->GetRepeatedString(message, descriptor, i)
                      : reflection->GetString(message, descriptor);
          break;
        case Field::CPPTYPE_MESSAGE:
          value = encode(
              repeated ? reflection->GetRepeatedMessage(message, descriptor, i)
                       : reflection->GetMessage(message, descriptor),
              depth + 1);
          break;
      }
      values.push_back(std::move(value));
    }
    if (unordered(*descriptor)) {
      std::sort(values.begin(), values.end());
    }
    for (const auto& value : values) {
      result += field(descriptor->number(), value);
    }
  }
  return result;
}
/** Uses the same builtin identities as a fresh semantic analysis session. */
const std::vector<moon::DeclarationRecord>& builtinDeclarations() {
  static const auto records = [] {
    auto results = std::make_shared<sun::semantic_analysis::AnalysisResults>();
    sun::semantic_analysis::SemanticContext context(results);
    const auto& table = results->declarations;
    std::vector<moon::DeclarationRecord> records;
    for (size_t i = 1; i <= table.size(); ++i) {
      const auto& source = table.get(sun::semantic_analysis::DeclarationId(i));
      if (!source.portableKey) {
        continue;
      }
      auto& record = records.emplace_back();
      record.set_key(source.portableKey->encoding());
      record.set_kind(static_cast<moon::DeclarationKind>(source.kind));
      record.set_name(source.name);
    }
    return records;
  }();
  return records;
}
/** Finds code that an importer must interpret, excluding module using records.
 */
bool hasRetainedCode(const google::protobuf::Message& message,
                     unsigned depth = 0) {
  if (depth > 128) {
    sun::support::logAndThrowError(
        "Unsupported retained metadata nesting depth");
  }
  if (message.GetDescriptor()->full_name() == "sun.proto.ast.ASTNode") {
    return true;
  }
  const auto* reflection = message.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->cpp_type() !=
            google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE ||
        field->name() == "using_declarations") {
      continue;
    }
    if (field->is_repeated()) {
      for (int i = 0; i < reflection->FieldSize(message, field); ++i) {
        if (hasRetainedCode(reflection->GetRepeatedMessage(message, field, i),
                            depth + 1)) {
          return true;
        }
      }
    } else if (hasRetainedCode(reflection->GetMessage(message, field),
                               depth + 1)) {
      return true;
    }
  }
  return false;
}

/** Checks portable references throughout private bodies and nested types. */
void validateReferences(
    const google::protobuf::Message& message,
    const std::map<std::string, const moon::DeclarationRecord*>& records,
    const std::set<std::string>& bundleScopes, bool completeImportSet,
    unsigned depth) {
  if (depth > 128) {
    sun::support::logAndThrowError(
        "Unsupported retained metadata nesting depth");
  }
  const auto* descriptor = message.GetDescriptor();
  const auto* reflection = message.GetReflection();
  // Compiled specializations introduce derived identities; they do not refer
  // to source declaration records. Their method symbols are checked by Moon.
  if (descriptor->full_name() == "sun.proto.ast.CompiledSpecialization") {
    const auto* key = descriptor->FindFieldByName("declaration_key");
    if (reflection->GetString(message, key).empty()) {
      sun::support::logAndThrowError("Compiled specialization has no identity");
    }
    return;
  }
  const bool needsNavigation =
      descriptor->full_name() != "sun.moon.ModuleMetadata" ||
      hasRetainedCode(message);
  const bool identity =
      descriptor->full_name() == "sun.proto.ast.DeclarationIdentity";
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    const bool repeated = field->is_repeated();
    const int count = repeated ? reflection->FieldSize(message, field) : 1;
    for (int i = 0; i < count; ++i) {
      if (field->cpp_type() ==
          google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        validateReferences(
            repeated ? reflection->GetRepeatedMessage(message, field, i)
                     : reflection->GetMessage(message, field),
            records, bundleScopes,
            completeImportSet &&
                (field->name() != "using_declarations" || needsNavigation),
            depth + 1);
      } else if (field->cpp_type() ==
                     google::protobuf::FieldDescriptor::CPPTYPE_STRING &&
                 (identity || field->name() == "declaration_key" ||
                  field->name() == "module_declaration_key")) {
        const auto key = repeated
                             ? reflection->GetRepeatedString(message, field, i)
                             : reflection->GetString(message, field);
        const auto found = records.find(key);
        // A single bundle can refer to another bundle. Resolve those keys
        // when the complete import set is available, before creating ASTs.
        if (!completeImportSet && found == records.end() && key.size() > 67 &&
            key[0] == '$' && key.substr(65, 2) == "$_" &&
            key.substr(1, 64).find_first_not_of("0123456789abcdef") ==
                std::string::npos &&
            !bundleScopes.count(key.substr(0, 67))) {
          continue;
        }
        if (found == records.end()) {
          std::string reason =
              "moon exact dependency: Missing retained declaration reference "
              "in " +
              field->full_name() + ": " + key;
          const auto* name = descriptor->FindFieldByName(
              descriptor->full_name() == "sun.proto.ast.TypeAnnotation"
                  ? "base_name"
                  : "name");
          if (name &&
              name->cpp_type() ==
                  google::protobuf::FieldDescriptor::CPPTYPE_STRING &&
              !name->is_repeated()) {
            const auto display = reflection->GetString(message, name);
            for (const auto& [candidate, record] : records) {
              if (!display.empty() &&
                  (display == record->name() ||
                   display.ends_with("." + record->name()))) {
                reason += " (conflicting bundle supplied)";
                break;
              }
            }
          }
          sun::support::logAndThrowError(
              reason + ". Explicitly import the required exact bundle.");
        }
        const auto kind = found->second->kind();
        if ((field->name() == "module_declaration_key" &&
             kind != moon::MODULE) ||
            (identity && field->name() == "parameters" &&
             kind != moon::PARAMETER) ||
            (identity && field->name() == "type_parameters" &&
             kind != moon::TYPE_PARAMETER) ||
            (identity && field->name() == "lifetime_parameters" &&
             kind != moon::LIFETIME_PARAMETER)) {
          sun::support::logAndThrowError(
              "Invalid retained declaration kind in " + field->full_name() +
              ": " + key);
        }
      }
    }
  }
}
}  // namespace

std::string describeSemantics(const moon::ModuleMetadata& metadata) {
  return field(0, "sun.abi.metadata.v1") + encode(metadata, 0);
}
void validateDeclarationGraph(
    const std::vector<const moon::ModuleMetadata*>& modules,
    bool completeImportSet) {
  std::map<std::string, const moon::DeclarationRecord*> records;
  for (const auto& record : builtinDeclarations()) {
    records.emplace(record.key(), &record);
  }
  for (const auto* module : modules) {
    for (const auto& record : module->declarations()) {
      if (record.key().empty() ||
          !moon::DeclarationKind_IsValid(record.kind())) {
        sun::support::logAndThrowError("Invalid declaration identity or kind");
      }
      const auto [found, inserted] = records.emplace(record.key(), &record);
      if (!inserted && (!completeImportSet ||
                        encode(*found->second, 0) != encode(record, 0))) {
        sun::support::logAndThrowError("Duplicate declaration identity: " +
                                       record.key());
      }
    }
  }
  std::set<std::string> bundleScopes;
  for (const auto& [key, record] : records) {
    if (key.size() > 67 && key[0] == '$' && key.substr(65, 2) == "$_") {
      bundleScopes.insert(key.substr(0, 67));
    }
  }
  for (const auto* module : modules) {
    validateReferences(*module, records, bundleScopes, completeImportSet, 0);
  }
  std::map<std::string, size_t> pending;
  std::map<std::string, std::vector<std::string>> dependents;
  std::deque<std::string> ready;
  for (const auto& [key, record] : records) {
    auto& count = pending[key];
    for (const auto* reference : {&record->owner(), &record->module()}) {
      if (reference->empty()) {
        continue;
      }
      const auto found = records.find(*reference);
      if (found == records.end()) {
        sun::support::logAndThrowError("Missing declaration owner or module: " +
                                       *reference);
      }
      if (reference == &record->module() &&
          found->second->kind() != moon::MODULE) {
        sun::support::logAndThrowError(
            "Declaration module reference does not name a module");
      }
      ++count;
      dependents[*reference].push_back(key);
    }
    if (!count) {
      ready.push_back(key);
    }
  }
  size_t completed = 0;
  while (!ready.empty()) {
    const auto key = std::move(ready.front());
    ready.pop_front();
    ++completed;
    for (const auto& dependent : dependents[key]) {
      if (--pending[dependent] == 0) {
        ready.push_back(dependent);
      }
    }
  }
  if (completed != records.size()) {
    sun::support::logAndThrowError("Declaration ownership contains a cycle");
  }
  for (const auto* module : modules) {
    for (const auto& key : module->module_declarations()) {
      const auto found = records.find(key);
      if (found == records.end() || found->second->kind() != moon::MODULE) {
        sun::support::logAndThrowError(
            "Missing or invalid indexed module declaration: " + key);
      }
    }
  }
}
void validateSpecializationPayload(const moon::ModuleMetadata& metadata,
                                   const llvm::Module& module) {
  for (const auto& cls : metadata.classes()) {
    for (const auto& specialization : cls.compiled_specializations()) {
      for (const auto& symbol : specialization.method_symbols()) {
        const auto* function = module.getFunction(symbol);
        if (!function || function->isDeclaration()) {
          sun::support::logAndThrowError(
              "Compiled specialization '" + specialization.declaration_key() +
              "' has no method definition for '" + symbol + "'");
        }
      }
    }
  }
}
}  // namespace sun::abi
