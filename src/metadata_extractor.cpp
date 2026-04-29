#include "metadata_extractor.h"

#include <llvm/Support/SHA256.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "parser.h"

namespace sun {

namespace {

bool isPrimitiveOrBuiltinType(const std::string& name) {
  static const std::set<std::string> primitives = {
      "void", "bool", "i8",  "i16", "i32",    "i64",   "u8",    "u16",
      "u32",  "u64",  "f32", "f64", "string", "slice", "IError"};
  static const std::set<std::string> typeKeywords = {
      "raw_ptr", "static_ptr", "ref", "fn", "lambda", "array"};
  return primitives.count(name) > 0 || typeKeywords.count(name) > 0;
}

std::string qualifyTypeAnnotation(const TypeAnnotation& ta,
                                  const std::string& nsPrefix,
                                  const std::set<std::string>& typeParams) {
  if (ta.isArray() && ta.elementType) {
    std::string result =
        "array<" + qualifyTypeAnnotation(*ta.elementType, nsPrefix, typeParams);
    for (size_t dim : ta.arrayDimensions) {
      result += ", " + std::to_string(dim);
    }
    result += ">";
    if (ta.canError) result += ", error";
    return result;
  }

  if (ta.isRawPointer() && ta.elementType) {
    return "raw_ptr(" +
           qualifyTypeAnnotation(*ta.elementType, nsPrefix, typeParams) + ")";
  }
  if (ta.isStaticPointer() && ta.elementType) {
    return "static_ptr(" +
           qualifyTypeAnnotation(*ta.elementType, nsPrefix, typeParams) + ")";
  }
  if (ta.isReference() && ta.elementType) {
    return "ref(" +
           qualifyTypeAnnotation(*ta.elementType, nsPrefix, typeParams) + ")";
  }

  if (ta.isFunction()) {
    std::string result = "_(";
    for (size_t i = 0; i < ta.paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += qualifyTypeAnnotation(*ta.paramTypes[i], nsPrefix, typeParams);
    }
    result += ") -> ";
    result += ta.returnType
                  ? qualifyTypeAnnotation(*ta.returnType, nsPrefix, typeParams)
                  : "void";
    return result;
  }

  if (ta.isLambda()) {
    std::string result = "(";
    for (size_t i = 0; i < ta.paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += qualifyTypeAnnotation(*ta.paramTypes[i], nsPrefix, typeParams);
    }
    result += ") -> ";
    result += ta.returnType
                  ? qualifyTypeAnnotation(*ta.returnType, nsPrefix, typeParams)
                  : "void";
    return result;
  }

  if (!ta.typeArguments.empty()) {
    std::string baseName = ta.baseName;
    if (!nsPrefix.empty() && !isPrimitiveOrBuiltinType(baseName) &&
        typeParams.count(baseName) == 0) {
      baseName = nsPrefix + baseName;
    }
    std::string result = baseName + "<";
    for (size_t i = 0; i < ta.typeArguments.size(); ++i) {
      if (i > 0) result += ", ";
      result +=
          qualifyTypeAnnotation(*ta.typeArguments[i], nsPrefix, typeParams);
    }
    result += ">";
    if (ta.canError) result += ", error";
    return result;
  }

  std::string result = ta.baseName;
  if (!nsPrefix.empty() && !isPrimitiveOrBuiltinType(result) &&
      typeParams.count(result) == 0) {
    result = nsPrefix + result;
  }
  if (ta.canError) result += ", error";
  return result;
}

std::string qualifyTypeAnnotation(
    const TypeAnnotation& ta, const std::string& nsPrefix,
    const std::vector<std::string>& typeParamVec) {
  std::set<std::string> typeParams(typeParamVec.begin(), typeParamVec.end());
  return qualifyTypeAnnotation(ta, nsPrefix, typeParams);
}

std::string qualifyOptTypeAnnotation(
    const std::optional<TypeAnnotation>& ta, const std::string& nsPrefix,
    const std::vector<std::string>& typeParams) {
  return ta ? qualifyTypeAnnotation(*ta, nsPrefix, typeParams) : "void";
}

void extractFunction(const FunctionAST& func, ModuleMetadata& metadata,
                     const std::string& nsPrefix) {
  const auto& proto = func.getProto();

  ExportedSymbol sym;
  sym.kind = ExportedSymbol::Kind::Function;
  sym.baseName = proto.getName();
  sym.qualifiedName =
      nsPrefix.empty() ? proto.getName() : nsPrefix + proto.getName();

  std::vector<std::string> typeParams = proto.getTypeParameters();

  // Use empty prefix for type signatures - types use base names in metadata
  std::string sig = "(";
  const auto& args = proto.getArgs();
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) sig += ", ";
    sig += qualifyTypeAnnotation(args[i].second, "", typeParams);
  }
  sig +=
      ") -> " + qualifyOptTypeAnnotation(proto.getReturnType(), "", typeParams);
  sym.typeSignature = sig;
  sym.isPublic = true;

  metadata.exports.push_back(sym);
}

void extractClass(const ClassDefinitionAST& classDef, ModuleMetadata& metadata,
                  const std::string& nsPrefix, const std::string& sourceHash) {
  ClassInfo classInfo;
  classInfo.baseName = classDef.getName();
  classInfo.qualifiedName =
      nsPrefix.empty() ? classDef.getName() : nsPrefix + classDef.getName();
  classInfo.sourceHash = sourceHash;
  classInfo.typeParams = classDef.getTypeParameters();

  std::vector<std::string> classTypeParams = classDef.getTypeParameters();

  for (const auto& iface : classDef.getImplementedInterfaces()) {
    // Use base name - interfaces are loaded inside a namespace block
    std::string ifaceStr = iface.name;
    if (!iface.typeArguments.empty()) {
      ifaceStr += "<";
      for (size_t i = 0; i < iface.typeArguments.size(); ++i) {
        if (i > 0) ifaceStr += ", ";
        ifaceStr +=
            qualifyTypeAnnotation(iface.typeArguments[i], "", classTypeParams);
      }
      ifaceStr += ">";
    }
    classInfo.interfaces.push_back(ifaceStr);
  }

  for (const auto& field : classDef.getFields()) {
    FieldInfo fieldInfo;
    fieldInfo.name = field.name;
    fieldInfo.typeSig = qualifyTypeAnnotation(field.type, "", classTypeParams);
    classInfo.fields.push_back(fieldInfo);
  }

  for (const auto& method : classDef.getMethods()) {
    MethodInfo methodInfo;
    const auto& proto = method.function->getProto();
    methodInfo.name = proto.getName();

    std::vector<std::string> methodTypeParams = classTypeParams;
    for (const auto& tp : proto.getTypeParameters()) {
      methodTypeParams.push_back(tp);
      methodInfo.typeParams.push_back(tp);
    }

    methodInfo.returnTypeSig =
        qualifyOptTypeAnnotation(proto.getReturnType(), "", methodTypeParams);
    methodInfo.isStatic = false;

    if (proto.hasVariadicParam()) {
      methodInfo.variadicParamName = proto.getVariadicParamName().value_or("");
      if (proto.hasVariadicConstraint()) {
        methodInfo.variadicConstraint = qualifyTypeAnnotation(
            *proto.getVariadicConstraint(), "", methodTypeParams);
      }
    }

    if (method.function->hasSourceText()) {
      methodInfo.bodySource = method.function->getSourceText();
    }

    for (const auto& param : proto.getArgs()) {
      methodInfo.paramNames.push_back(param.first);
      methodInfo.paramTypeSigs.push_back(
          qualifyTypeAnnotation(param.second, "", methodTypeParams));
    }

    classInfo.methods.push_back(methodInfo);
  }

  metadata.classes.push_back(classInfo);

  ExportedSymbol sym;
  sym.kind = ExportedSymbol::Kind::Class;
  sym.baseName = classInfo.baseName;
  sym.qualifiedName = classInfo.qualifiedName + "_struct";
  sym.isPublic = true;
  metadata.exports.push_back(sym);
}

void extractInterface(const InterfaceDefinitionAST& ifaceDef,
                      ModuleMetadata& metadata, const std::string& nsPrefix) {
  InterfaceInfo ifaceInfo;
  ifaceInfo.baseName = ifaceDef.getName();
  ifaceInfo.qualifiedName =
      nsPrefix.empty() ? ifaceDef.getName() : nsPrefix + ifaceDef.getName();

  std::vector<std::string> ifaceTypeParams = ifaceDef.getTypeParameters();

  for (const auto& method : ifaceDef.getMethods()) {
    MethodInfo methodInfo;
    const auto& proto = method.function->getProto();
    methodInfo.name = proto.getName();

    std::vector<std::string> methodTypeParams = ifaceTypeParams;
    for (const auto& tp : proto.getTypeParameters()) {
      methodTypeParams.push_back(tp);
      methodInfo.typeParams.push_back(tp);
    }

    methodInfo.returnTypeSig =
        qualifyOptTypeAnnotation(proto.getReturnType(), "", methodTypeParams);

    if (proto.hasVariadicParam()) {
      methodInfo.variadicParamName = proto.getVariadicParamName().value_or("");
      if (proto.hasVariadicConstraint()) {
        methodInfo.variadicConstraint = qualifyTypeAnnotation(
            *proto.getVariadicConstraint(), "", methodTypeParams);
      }
    }

    if (method.function->hasSourceText()) {
      methodInfo.bodySource = method.function->getSourceText();
    }

    for (const auto& param : proto.getArgs()) {
      methodInfo.paramNames.push_back(param.first);
      methodInfo.paramTypeSigs.push_back(
          qualifyTypeAnnotation(param.second, "", methodTypeParams));
    }

    ifaceInfo.methods.push_back(methodInfo);
  }

  metadata.interfaces.push_back(ifaceInfo);

  ExportedSymbol sym;
  sym.kind = ExportedSymbol::Kind::Interface;
  sym.baseName = ifaceInfo.baseName;
  sym.qualifiedName = ifaceInfo.qualifiedName;
  sym.isPublic = true;
  metadata.exports.push_back(sym);
}

void extractFromStatements(const std::vector<std::unique_ptr<ExprAST>>& stmts,
                           ModuleMetadata& metadata,
                           const std::string& nsPrefix,
                           const std::filesystem::path& moduleDir,
                           const std::string& sourceHash) {
  for (const auto& stmt : stmts) {
    if (!stmt) continue;

    if (stmt->isImport() && nsPrefix.empty()) {
      const auto& importStmt = static_cast<const ImportAST&>(*stmt);
      std::filesystem::path depPath = importStmt.getPath();
      if (depPath.is_relative()) {
        depPath = std::filesystem::weakly_canonical(moduleDir / depPath);
      }
      metadata.dependencies.push_back(depPath.string());
    }

    if (stmt->getType() == ASTNodeType::NAMESPACE) {
      const auto& nsDecl = static_cast<const NamespaceAST&>(*stmt);
      std::string newPrefix = nsPrefix.empty()
                                  ? nsDecl.getName() + "_"
                                  : nsPrefix + nsDecl.getName() + "_";
      if (nsPrefix.empty() && metadata.moduleName.empty()) {
        metadata.moduleName = nsDecl.getName();
      }
      extractFromStatements(nsDecl.getBody().getBody(), metadata, newPrefix,
                            moduleDir, sourceHash);
    }

    if (stmt->getType() == ASTNodeType::FUNCTION) {
      extractFunction(static_cast<const FunctionAST&>(*stmt), metadata,
                      nsPrefix);
    }

    if (stmt->getType() == ASTNodeType::CLASS_DEFINITION) {
      extractClass(static_cast<const ClassDefinitionAST&>(*stmt), metadata,
                   nsPrefix, sourceHash);
    }

    if (stmt->getType() == ASTNodeType::INTERFACE_DEFINITION) {
      extractInterface(static_cast<const InterfaceDefinitionAST&>(*stmt),
                       metadata, nsPrefix);
    }
  }
}

ModuleMetadata extractMetadata(const std::string& filePath,
                               const BlockExprAST& ast,
                               const std::string& sourceHash) {
  ModuleMetadata metadata;
  metadata.sourceHash = sourceHash;
  metadata.version = "1.0.0";

  std::filesystem::path moduleDir =
      std::filesystem::path(filePath).parent_path();

  extractFromStatements(ast.getBody(), metadata, "", moduleDir, sourceHash);

  return metadata;
}

}  // namespace

std::optional<ModuleMetadata> extractMetadataFromFile(
    const std::string& filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  // Compute SHA-256 hash of source contents
  llvm::SHA256 sha;
  sha.update(llvm::StringRef(source));
  auto hashBytes = sha.final();
  std::string sourceHash;
  sourceHash.reserve(64);
  for (uint8_t b : hashBytes) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", b);
    sourceHash += hex;
  }

  std::istringstream ss(source);
  Parser parser(ss);
  parser.setBaseDir(std::filesystem::path(filename).parent_path().string());
  parser.getNextToken();

  auto ast = parser.parseProgram();
  if (!ast) {
    return std::nullopt;
  }

  return extractMetadata(filename, *ast, sourceHash);
}

}  // namespace sun
