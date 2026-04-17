#include "module_types.h"

#include <sstream>

namespace sun {

void ModuleTypeResolver::skipWhitespace(const std::string& sig, size_t& pos) {
  while (pos < sig.size() && (sig[pos] == ' ' || sig[pos] == '\t')) {
    ++pos;
  }
}

bool ModuleTypeResolver::match(const std::string& sig, size_t& pos, char c) {
  skipWhitespace(sig, pos);
  if (pos < sig.size() && sig[pos] == c) {
    ++pos;
    return true;
  }
  return false;
}

std::string ModuleTypeResolver::parseIdentifier(const std::string& sig,
                                                size_t& pos) {
  skipWhitespace(sig, pos);
  size_t start = pos;
  while (pos < sig.size() && (std::isalnum(sig[pos]) || sig[pos] == '_')) {
    ++pos;
  }
  return sig.substr(start, pos - start);
}

TypePtr ModuleTypeResolver::parseType(const std::string& sig, size_t& pos) {
  skipWhitespace(sig, pos);

  if (pos >= sig.size()) {
    return nullptr;
  }

  // Function type: (params) -> return
  if (sig[pos] == '(') {
    return parseFunctionType(sig, pos);
  }

  // Identifier (primitive type or class name)
  std::string name = parseIdentifier(sig, pos);
  if (name.empty()) {
    return nullptr;
  }

  // Check for primitive types
  if (name == "void") return std::make_shared<PrimitiveType>(Type::Kind::Void);
  if (name == "bool") return std::make_shared<PrimitiveType>(Type::Kind::Bool);
  if (name == "i8") return std::make_shared<PrimitiveType>(Type::Kind::Int8);
  if (name == "i16") return std::make_shared<PrimitiveType>(Type::Kind::Int16);
  if (name == "i32") return std::make_shared<PrimitiveType>(Type::Kind::Int32);
  if (name == "i64") return std::make_shared<PrimitiveType>(Type::Kind::Int64);
  if (name == "u8") return std::make_shared<PrimitiveType>(Type::Kind::UInt8);
  if (name == "u16") return std::make_shared<PrimitiveType>(Type::Kind::UInt16);
  if (name == "u32") return std::make_shared<PrimitiveType>(Type::Kind::UInt32);
  if (name == "u64") return std::make_shared<PrimitiveType>(Type::Kind::UInt64);
  if (name == "f32")
    return std::make_shared<PrimitiveType>(Type::Kind::Float32);
  if (name == "f64")
    return std::make_shared<PrimitiveType>(Type::Kind::Float64);

  // Check for generic type: Name<T1, T2> or Name(T1, T2)
  // The parenthesized form is used in serialized type signatures
  skipWhitespace(sig, pos);
  if (pos < sig.size() && (sig[pos] == '<' || sig[pos] == '(')) {
    return parseGenericType(name, sig, pos);
  }

  // Non-generic class/interface type
  return std::make_shared<ClassType>(name);
}

TypePtr ModuleTypeResolver::parseFunctionType(const std::string& sig,
                                              size_t& pos) {
  // Expect '('
  if (!match(sig, pos, '(')) {
    return nullptr;
  }

  // Parse parameter types
  std::vector<TypePtr> paramTypes;
  skipWhitespace(sig, pos);

  if (pos < sig.size() && sig[pos] != ')') {
    // At least one parameter
    auto paramType = parseType(sig, pos);
    if (!paramType) return nullptr;
    paramTypes.push_back(paramType);

    // More parameters?
    while (match(sig, pos, ',')) {
      paramType = parseType(sig, pos);
      if (!paramType) return nullptr;
      paramTypes.push_back(paramType);
    }
  }

  // Expect ')'
  if (!match(sig, pos, ')')) {
    return nullptr;
  }

  // Expect '->'
  skipWhitespace(sig, pos);
  if (pos + 1 < sig.size() && sig[pos] == '-' && sig[pos + 1] == '>') {
    pos += 2;
  } else {
    return nullptr;
  }

  // Parse return type
  auto returnType = parseType(sig, pos);
  if (!returnType) {
    // Default to void if no return type
    returnType = std::make_shared<PrimitiveType>(Type::Kind::Void);
  }

  return std::make_shared<FunctionType>(returnType, std::move(paramTypes));
}

TypePtr ModuleTypeResolver::parseGenericType(const std::string& name,
                                             const std::string& sig,
                                             size_t& pos) {
  // Accept '<' or '(' as opening bracket (serialized sigs use parens)
  skipWhitespace(sig, pos);
  char openBracket = (pos < sig.size()) ? sig[pos] : '\0';
  char closeBracket = (openBracket == '(') ? ')' : '>';
  if (openBracket != '<' && openBracket != '(') {
    return nullptr;
  }
  ++pos;  // consume opening bracket

  // Parse type arguments
  std::vector<TypePtr> typeArgs;
  skipWhitespace(sig, pos);

  if (pos < sig.size() && sig[pos] != closeBracket) {
    auto typeArg = parseType(sig, pos);
    if (!typeArg) return nullptr;
    typeArgs.push_back(typeArg);

    while (match(sig, pos, ',')) {
      typeArg = parseType(sig, pos);
      if (!typeArg) return nullptr;
      typeArgs.push_back(typeArg);
    }
  }

  // Expect closing bracket
  if (!match(sig, pos, closeBracket)) {
    return nullptr;
  }

  // Handle known generic types
  if (name == "raw_ptr" && typeArgs.size() == 1) {
    return std::make_shared<RawPointerType>(typeArgs[0]);
  }
  if (name == "ref" && typeArgs.size() == 1) {
    return std::make_shared<ReferenceType>(typeArgs[0]);
  }
  if (name == "static_ptr" && typeArgs.size() == 1) {
    return std::make_shared<StaticPointerType>(typeArgs[0]);
  }

  // For generic class types like MatrixView<u8>, create a proper ClassType
  // with mangled name (e.g., MatrixView_u8) so it matches the specialized class
  std::string mangledName = Types::mangleGenericClassName(name, typeArgs);
  return std::make_shared<ClassType>(mangledName, name, typeArgs);
}

TypePtr ModuleTypeResolver::parseTypeSignature(const std::string& sig) {
  size_t pos = 0;
  return parseType(sig, pos);
}

std::string ModuleTypeResolver::serializeType(const TypePtr& type) {
  if (!type) return "void";
  return type->toString();
}

std::unordered_map<std::string, TypePtr> ModuleTypeResolver::buildExportTable(
    const ModuleMetadata& metadata) {
  std::unordered_map<std::string, TypePtr> exports;

  for (const auto& sym : metadata.exports) {
    if (sym.kind == ExportedSymbol::Kind::Function) {
      auto type = parseTypeSignature(sym.typeSignature);
      if (type) {
        exports[sym.name] = type;
      }
    }
  }

  return exports;
}

}  // namespace sun
