#include <map>
#include <set>

#include "abi/lowering_descriptors.h"
#include "abi/semantic_requirements.h"
#include "codegen/llvm_type_resolver.h"
#include "semantic_analysis/portable_declaration_key.h"
#include "support/error.h"
#include "types/types.h"

/** Checks decoded source declarations against independently lowered signatures.
 */
namespace sun::abi {
/** Reconstructs metadata types without borrowing types from the supplied
 * payload. */
namespace {
/** Keeps generic arguments symbolic until their storage is needed. */
using Bindings = std::map<std::string, sun::proto::ast::TypeAnnotation>;

/** Substitutes source types without expanding nominal storage or pointer
 * targets. */
sun::proto::ast::TypeAnnotation substitute(
    const sun::proto::ast::TypeAnnotation& annotation, const Bindings& bindings,
    unsigned depth = 0) {
  if (depth > 128) {
    sun::support::logAndThrowError("Unsupported ABI type nesting depth");
  }
  if (!annotation.has_declaration_key()) {
    if (const auto found = bindings.find(annotation.base_name());
        found != bindings.end()) {
      return found->second;
    }
  }
  auto result = annotation;
  if (result.has_element_type()) {
    *result.mutable_element_type() =
        substitute(annotation.element_type(), bindings, depth + 1);
  }
  if (result.has_return_type()) {
    *result.mutable_return_type() =
        substitute(annotation.return_type(), bindings, depth + 1);
  }
  for (auto& type : *result.mutable_param_types()) {
    type = substitute(type, bindings, depth + 1);
  }
  for (auto& type : *result.mutable_type_arguments()) {
    type = substitute(type, bindings, depth + 1);
  }
  return result;
}

/** Reconstructs storage for locally defined concrete classes and primitive
 * types. */
class MetadataLayouts {
  llvm::LLVMContext& context_;
  sun::codegen::LLVMTypeResolver resolver_;
  const llvm::DataLayout& layout_;
  std::map<std::string, const sun::proto::ast::ClassDef*> classes_;
  std::map<std::string, const sun::proto::ast::EnumDef*> enums_;
  std::set<std::string> interfaces_;
  std::set<std::string> active_;

 public:
  /** Collects declarations from every metadata module sharing the payload. */
  MetadataLayouts(const std::vector<const moon::ModuleMetadata*>& modules,
                  llvm::LLVMContext& context, const llvm::DataLayout& layout)
      : context_(context), resolver_(context, &layout), layout_(layout) {
    for (const auto* module : modules) {
      for (const auto& cls : module->classes()) {
        classes_.emplace(cls.declaration_identity().key(), &cls);
      }
      for (const auto& interface : module->interfaces()) {
        interfaces_.insert(interface.declaration_identity().key());
      }
      for (const auto& enumeration : module->enums()) {
        enums_.emplace(enumeration.declaration_identity().key(), &enumeration);
      }
    }
  }

  /** Returns storage when metadata fully specifies it; unresolved forms stay
   * experimental. */
  llvm::Type* decode(const sun::proto::ast::TypeAnnotation& annotation,
                     bool returned = false, const Bindings& bindings = {},
                     unsigned depth = 0) {
    if (depth > 128) {
      sun::support::logAndThrowError("Unsupported ABI type nesting depth");
    }
    if (!bindings.empty()) {
      return decode(substitute(annotation, bindings), returned, {}, depth + 1);
    }
    if (annotation.can_error()) {
      return nullptr;
    }
    const auto& name = annotation.base_name();
    if (!annotation.has_declaration_key()) {
      if (auto primitive = sun::types::Types::fromString(name)) {
        return resolver_.resolve(primitive);
      }
    }
    auto* pointer = llvm::PointerType::getUnqual(context_);
    if (name == "raw_ptr" || name == "fn") {
      return pointer;
    }
    if (name == "lambda") {
      return resolver_.getClosureType();
    }
    if (name == "static_ptr") {
      return resolver_.getStaticPtrType();
    }
    if (name == "ref") {
      const auto& element = annotation.element_type();
      if (element.base_name() == "array" &&
          element.array_dimensions().empty()) {
        return sun::types::ArrayType::getArrayStructType(context_);
      }
      if (returned && (interfaces_.contains(element.declaration_key()) ||
                       (!element.has_declaration_key() &&
                        element.base_name() == "IError"))) {
        return sun::types::InterfaceType::getFatPointerType(context_);
      }
      // An unavailable nominal declaration may be an interface borrow returned
      // by value. Check it once its source layout is available, not as a
      // pointer.
      if (returned && element.has_declaration_key() &&
          !classes_.contains(element.declaration_key())) {
        return nullptr;
      }
      return pointer;
    }
    if (name == "array") {
      if (annotation.array_dimensions().empty()) {
        return sun::types::ArrayType::getArrayStructType(context_);
      }
      auto* element =
          decode(annotation.element_type(), false, bindings, depth + 1);
      if (!element || !element->isSized()) {
        return nullptr;
      }
      for (auto i = annotation.array_dimensions().rbegin();
           i != annotation.array_dimensions().rend(); ++i) {
        if (!i->has_size()) {
          return nullptr;
        }
        element = llvm::ArrayType::get(element, i->size());
      }
      return element;
    }
    if (interfaces_.contains(annotation.declaration_key()) ||
        (!annotation.has_declaration_key() && name == "IError")) {
      return sun::types::InterfaceType::getFatPointerType(context_);
    }
    if (const auto enumeration = enums_.find(annotation.declaration_key());
        enumeration != enums_.end()) {
      const auto& definition = *enumeration->second;
      if (definition.type_params_size() != annotation.type_arguments_size()) {
        return nullptr;
      }
      Bindings arguments;
      std::string identity = annotation.declaration_key();
      for (int i = 0; i < definition.type_params_size(); ++i) {
        auto type = substitute(annotation.type_arguments(i), bindings);
        const auto encoded = type.SerializeAsString();
        identity += ":" + std::to_string(encoded.size()) + ":" + encoded;
        arguments.emplace(definition.type_params(i).name(), std::move(type));
      }
      if (!active_.insert(identity).second) {
        sun::support::logAndThrowError("Recursive by-value ABI enum layout: " +
                                       definition.name());
      }
      std::vector<std::vector<llvm::Type*>> payloads;
      bool hasPayload = false;
      for (const auto& variant : definition.variants()) {
        std::vector<llvm::Type*> fields;
        for (const auto& field : variant.payload_types()) {
          auto* type = decode(field, true, arguments, depth + 1);
          if (!type || !type->isSized()) {
            active_.erase(identity);
            return nullptr;
          }
          fields.push_back(type);
          hasPayload = true;
        }
        payloads.push_back(std::move(fields));
      }
      active_.erase(identity);
      if (hasPayload) {
        return EnumLayout::build(context_, layout_, payloads).storage;
      }
      auto underlying = sun::types::Types::fromString(
          definition.underlying_type().empty() ? "i32"
                                               : definition.underlying_type());
      if (!underlying || !underlying->isIntegral()) {
        sun::support::logAndThrowError("Invalid ABI enum representation");
      }
      return resolver_.resolve(underlying);
    }
    const auto found = classes_.find(annotation.declaration_key());
    if (found == classes_.end()) {
      return nullptr;
    }
    const auto& cls = *found->second;
    if (cls.type_params_size() != annotation.type_arguments_size()) {
      return nullptr;
    }
    Bindings arguments;
    std::string identity = annotation.declaration_key();
    for (int i = 0; i < cls.type_params_size(); ++i) {
      auto type = substitute(annotation.type_arguments(i), bindings);
      const auto encoded = type.SerializeAsString();
      identity += ":" + std::to_string(encoded.size()) + ":" + encoded;
      arguments.emplace(cls.type_params(i).name(), std::move(type));
    }
    if (!active_.insert(identity).second) {
      sun::support::logAndThrowError("Recursive by-value ABI class layout: " +
                                     cls.name());
    }
    std::vector<llvm::Type*> fields;
    for (const auto& field : cls.fields()) {
      auto* type = decode(field.type(), false, arguments, depth + 1);
      if (!type || !type->isSized()) {
        active_.erase(identity);
        return nullptr;
      }
      fields.push_back(type);
    }
    active_.erase(identity);
    return AggregateLayout::build(context_, fields, cls.is_packed(),
                                  cls.alignment());
  }
};

/** Checks a concrete Sun function when all of its machine types are
 * reconstructable. */
void checkFunction(const sun::proto::ast::FunctionDef& declaration,
                   const llvm::Module& payload, MetadataLayouts& layouts,
                   bool method = false) {
  const auto& proto = declaration.proto();
  if (declaration.is_c_extern() || declaration.is_test() ||
      proto.type_params_size() || proto.type_parameters_size() ||
      proto.has_variadic_param_name() ||
      proto.declaration_identity().key().empty()) {
    return;
  }
  const auto symbol =
      sun::semantic_analysis::PortableDeclarationKey::fromString(
          proto.declaration_identity().key())
          .symbol("function");
  auto* result = layouts.decode(proto.return_type(), true);
  if (!result) {
    return;
  }
  std::vector<llvm::Type*> parameters;
  for (const auto& argument : proto.args()) {
    auto* type = layouts.decode(argument.type());
    if (!type || type->isVoidTy()) {
      return;
    }
    parameters.push_back(type);
  }
  const auto* function = payload.getFunction(symbol);
  if (!function || function->isDeclaration()) {
    sun::support::logAndThrowError("Missing compiled ABI function: " +
                                   proto.name());
  }
  const auto expected = CallDescriptor::sun(result, parameters, method);
  const auto actual = CallDescriptor::inspect(*function);
  if (machineTypeName(expected.signature) !=
          machineTypeName(actual.signature) ||
      expected.convention != actual.convention ||
      expected.encodedAttributes() != actual.encodedAttributes()) {
    sun::support::logAndThrowError(
        "Declaration '" + proto.name() + "' (" + symbol +
        ") does not match its compiled ABI signature");
  }
}
}  // namespace

void validatePayloadDeclarations(
    const std::vector<const moon::ModuleMetadata*>& modules,
    const llvm::Module& module) {
  llvm::LLVMContext context;
  MetadataLayouts layouts(modules, context, module.getDataLayout());
  const auto emittedLayouts = classLayouts(module);
  for (const auto* metadata : modules) {
    for (const auto& function : metadata->functions()) {
      checkFunction(function, module, layouts);
    }
    for (const auto& cls : metadata->classes()) {
      if (cls.type_params_size() || cls.type_parameters_size()) {
        continue;
      }
      const auto& identity = cls.declaration_identity().key();
      if (!identity.empty()) {
        const auto emitted = emittedLayouts.find(identity);
        if (emitted == emittedLayouts.end()) {
          sun::support::logAndThrowError("Missing compiled class ABI layout: " +
                                         cls.name());
        }
        sun::proto::ast::TypeAnnotation annotation;
        annotation.set_base_name(cls.name());
        annotation.set_declaration_key(identity);
        if (auto* expected = layouts.decode(annotation);
            expected &&
            machineTypeName(expected) != machineTypeName(emitted->second)) {
          sun::support::logAndThrowError(
              "Class '" + cls.name() +
              "' does not match its compiled ABI layout");
        }
      }
      for (const auto& method : cls.methods()) {
        checkFunction(method.function(), module, layouts, true);
      }
    }
  }
}
}  // namespace sun::abi
