// hover.cpp — Type information for the language server's hover request

#include "lsp/hover.h"

#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast.h"
#include "ast/ast_children.h"
#include "source_manager.h"

namespace sun::lsp {

namespace {

// Type parameter name -> the type it stands for in one specialization
using Bindings = std::vector<std::pair<std::string, sun::TypePtr>>;

std::string normalizePath(const std::string& path) {
  try {
    if (std::filesystem::exists(path)) {
      return std::filesystem::canonical(path).string();
    }
  } catch (const std::filesystem::filesystem_error&) {
  }
  return path;
}

bool spanContains(const Position& loc, int offset) {
  return loc.endOffset.has_value() && loc.offset <= offset &&
         offset < *loc.endOffset;
}

std::string sliceSpan(const std::string& source, const Position& loc) {
  if (!loc.endOffset || loc.offset < 0 || *loc.endOffset < loc.offset ||
      static_cast<size_t>(*loc.endOffset) > source.size()) {
    return "";
  }
  return source.substr(loc.offset, *loc.endOffset - loc.offset);
}

// ---------------------------------------------------------------------------
// Locating the hovered node
// ---------------------------------------------------------------------------

// Descends to the innermost node containing the offset, keeping its ancestors
class NodeFinder {
 public:
  NodeFinder(std::string documentPath, int offset)
      : documentPath_(std::move(documentPath)), offset_(offset) {}

  const std::vector<const ExprAST*>& chain() const { return chain_; }

  // True when the subtree rooted at node covers the offset
  bool visit(const ExprAST& node) {
    if (node.getType() == ASTNodeType::MOON_SCOPE) return false;
    const Position& loc = node.getLocation();
    if (!isDocumentFile(loc)) return false;
    // Nodes without a span (merged module wrappers) are looked through
    bool hasSpan = loc.endOffset.has_value();
    if (hasSpan && !spanContains(loc, offset_)) return false;
    if (hasSpan) chain_.push_back(&node);
    bool found = false;
    forEachChild(node, [&](const ExprAST& child) {
      if (!found) found = visit(child);
    });
    return hasSpan || found;
  }

 private:
  bool isDocumentFile(const Position& loc) {
    if (!loc.filePath) return true;
    auto cached = fileMatches_.find(*loc.filePath);
    if (cached != fileMatches_.end()) return cached->second;
    bool matches = *loc.filePath == documentPath_ ||
                   normalizePath(*loc.filePath) == documentPath_;
    fileMatches_.emplace(*loc.filePath, matches);
    return matches;
  }

  std::string documentPath_;
  int offset_;
  std::unordered_map<std::string, bool> fileMatches_;
  std::vector<const ExprAST*> chain_;
};

// The hovered node with its ancestors (outermost first) and the type
// parameter bindings in effect when it sits inside a generic body
struct Target {
  std::vector<const ExprAST*> chain;
  Bindings bindings;
  const ExprAST& node() const { return *chain.back(); }
};

// First specialization of a generic template, with its bindings; null when
// the template is not generic or was never used
const ExprAST* firstSpecialization(const ExprAST& node, Bindings& bindings) {
  if (node.getType() == ASTNodeType::CLASS_DEFINITION) {
    const auto& cls = static_cast<const ClassDefinitionAST&>(node);
    if (!cls.isGeneric() || cls.getSpecializations().empty()) return nullptr;
    const auto& spec = cls.getSpecializations().begin()->second;
    // Class-level bindings are recorded on every cloned method
    for (const auto& method : spec->getMethods()) {
      if (method.function && method.function->getProto().hasTypeBindings()) {
        bindings = method.function->getProto().getTypeBindings();
        break;
      }
    }
    return spec.get();
  }
  if (node.getType() == ASTNodeType::FUNCTION) {
    const auto& fn = static_cast<const FunctionAST&>(node);
    if (!fn.getProto().isGeneric() || fn.getSpecializations().empty()) {
      return nullptr;
    }
    const auto& spec = fn.getSpecializations().begin()->second;
    bindings = spec->getProto().getTypeBindings();
    return spec.get();
  }
  return nullptr;
}

std::optional<Target> locate(const BlockExprAST& program,
                             const std::string& documentPath, int offset) {
  NodeFinder finder(documentPath, offset);
  finder.visit(program);
  if (finder.chain().empty()) return std::nullopt;
  Target target{finder.chain(), {}};

  // Generic templates are analyzed only through their specializations. The
  // clones keep the template's source spans, so the same offset is looked
  // up inside the first specialization and its concrete types are read back
  // through the bindings. A hover on the template's own header stays on the
  // template, which still carries the annotations as written.
  for (size_t i = 0; i + 1 < target.chain.size(); ++i) {
    Bindings bindings;
    const ExprAST* specialization =
        firstSpecialization(*target.chain[i], bindings);
    if (!specialization) continue;
    NodeFinder inner(documentPath, offset);
    if (!inner.visit(*specialization) || inner.chain().empty()) break;
    std::vector<const ExprAST*> chain(target.chain.begin(),
                                      target.chain.begin() + i);
    chain.insert(chain.end(), inner.chain().begin(), inner.chain().end());
    target.chain = std::move(chain);
    target.bindings.insert(target.bindings.end(), bindings.begin(),
                           bindings.end());
  }
  return target;
}

// ---------------------------------------------------------------------------
// Rendering types and signatures
// ---------------------------------------------------------------------------

std::string renderType(const sun::Type& type, const Bindings& bindings);

// `(i32, bool) i32` with `, IError` when the callable may throw
std::string renderCallable(const std::vector<sun::TypePtr>& params,
                           const sun::TypePtr& returnType, bool canThrow,
                           const Bindings& bindings) {
  std::string out = "(";
  for (size_t i = 0; i < params.size(); ++i) {
    if (i > 0) out += ", ";
    out += params[i] ? renderType(*params[i], bindings) : "?";
  }
  out += ") ";
  out += returnType ? renderType(*returnType, bindings) : "void";
  if (canThrow) out += ", IError";
  return out;
}

// `Vec<T>`: the display name's base with each type argument re-rendered so
// bound type parameters show by name
std::string renderWithArguments(const sun::Type& type,
                                const std::vector<sun::TypePtr>& arguments,
                                const Bindings& bindings) {
  std::string display = type.toDisplayString();
  if (arguments.empty() || bindings.empty()) return display;
  std::string out = display.substr(0, display.find('<'));
  for (size_t i = 0; i < arguments.size(); ++i) {
    out += i == 0 ? "<" : ", ";
    out += arguments[i] ? renderType(*arguments[i], bindings) : "?";
  }
  return out + ">";
}

// Sun-syntax spelling of a type. A type bound to a type parameter prints as
// the parameter; function types print as written in Sun (`(i32) i32`);
// everything else uses the type's display name.
std::string renderType(const sun::Type& type, const Bindings& bindings) {
  for (const auto& [name, bound] : bindings) {
    if (bound && bound->equals(type)) return name;
  }
  switch (type.getKind()) {
    case sun::Type::Kind::Function: {
      const auto& fn = static_cast<const sun::FunctionType&>(type);
      return renderCallable(fn.getParamTypes(), fn.getReturnType(),
                            fn.canThrow(), bindings);
    }
    case sun::Type::Kind::Lambda: {
      const auto& lambda = static_cast<const sun::LambdaType&>(type);
      return renderCallable(lambda.getParamTypes(), lambda.getReturnType(),
                            lambda.canThrow(), bindings);
    }
    case sun::Type::Kind::Reference: {
      const auto& ref = static_cast<const sun::ReferenceType&>(type);
      if (!ref.getReferencedType()) return type.toDisplayString();
      return std::string(ref.isMutable() ? "ref " : "const ref ") +
             renderType(*ref.getReferencedType(), bindings);
    }
    case sun::Type::Kind::Class:
      return renderWithArguments(
          type, static_cast<const sun::ClassType&>(type).getTypeArguments(),
          bindings);
    case sun::Type::Kind::Interface:
      return renderWithArguments(
          type,
          static_cast<const sun::InterfaceType&>(type).getTypeArguments(),
          bindings);
    case sun::Type::Kind::Enum:
      return renderWithArguments(
          type, static_cast<const sun::EnumType&>(type).getGenericArgs(),
          bindings);
    default:
      return type.toDisplayString();
  }
}

// The annotation as the user wrote it, when the source span is available
std::string annotationText(const TypeAnnotation& annotation,
                           const std::string& source) {
  std::string text = sliceSpan(source, annotation.span);
  return text.empty() ? annotation.toString() : text;
}

std::string renderTypeParameters(const std::vector<std::string>& params) {
  if (params.empty()) return "";
  std::string out = "<";
  for (size_t i = 0; i < params.size(); ++i) {
    if (i > 0) out += ", ";
    out += params[i];
  }
  return out + ">";
}

// `function name(a: i32, b: i32) i32` — resolved types when the analyzer
// recorded them (specializations), otherwise the annotations as written
std::string renderPrototype(const PrototypeAST& proto,
                            const std::string& keyword,
                            const std::string& name, bool isPublic,
                            const std::string& source,
                            const Bindings& bindings) {
  std::string out;
  if (isPublic) out += "public ";
  if (proto.isConstMethod()) out += "const ";
  out += keyword;
  if (!name.empty()) out += " " + name;
  out += renderTypeParameters(proto.getTypeParameters());
  out += "(";

  const auto& args = proto.getArgs();
  const std::vector<sun::TypePtr>* resolved = nullptr;
  if (proto.hasResolvedParamTypes() &&
      proto.getResolvedParamTypes().size() == args.size()) {
    resolved = &proto.getResolvedParamTypes();
  }
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) out += ", ";
    out += args[i].first + ": ";
    if (resolved && (*resolved)[i]) {
      out += renderType(*(*resolved)[i], bindings);
    } else {
      out += annotationText(args[i].second, source);
    }
  }
  if (proto.hasVariadicParam()) {
    if (!args.empty()) out += ", ";
    out += *proto.getVariadicParamName();
    if (proto.hasVariadicConstraint()) {
      out += ": " + annotationText(*proto.getVariadicConstraint(), source);
    }
    out += "...";
  }
  out += ")";

  if (proto.hasResolvedReturnType()) {
    out += " " + renderType(*proto.getResolvedReturnType(), bindings);
    if (proto.canThrow()) out += ", IError";
  } else if (proto.hasReturnType()) {
    out += " " + annotationText(*proto.getReturnType(), source);
  }
  return out;
}

std::optional<Hover> hoverClass(const ClassDefinitionAST& cls, int offset,
                                const std::string& source,
                                const Bindings& bindings) {
  const auto* classType =
      dynamic_cast<const sun::ClassType*>(cls.getResolvedType().get());
  for (const auto& field : cls.getFields()) {
    Position span = field.location;
    if (field.type.span.endOffset) span.endOffset = field.type.span.endOffset;
    if (!spanContains(span, offset)) continue;
    std::string typeText;
    if (classType) {
      if (const auto* info = classType->getField(field.name);
          info && info->type) {
        typeText = renderType(*info->type, bindings);
      }
    }
    if (typeText.empty()) typeText = annotationText(field.type, source);
    std::string prefix =
        field.visibility == sun::Visibility::Public ? "public var " : "var ";
    return Hover{prefix + field.name + ": " + typeText, "", span};
  }

  std::string out = cls.isPublic() ? "public " : "";
  if (cls.isPartial()) out += "partial ";
  out += std::string(cls.classKeyword()) + " " + cls.getName() +
         renderTypeParameters(cls.getTypeParameters());
  const auto& interfaces = cls.getImplementedInterfaces();
  for (size_t i = 0; i < interfaces.size(); ++i) {
    out += i == 0 ? " implements " : ", ";
    out += interfaces[i].name;
    for (size_t j = 0; j < interfaces[i].typeArguments.size(); ++j) {
      out += j == 0 ? "<" : ", ";
      out += annotationText(interfaces[i].typeArguments[j], source);
    }
    if (!interfaces[i].typeArguments.empty()) out += ">";
  }
  return Hover{out, "", cls.getLocation()};
}

std::optional<Hover> hoverInterface(const InterfaceDefinitionAST& iface,
                                    int offset, const std::string& source) {
  for (const auto& field : iface.getFields()) {
    Position span = field.location;
    if (field.type.span.endOffset) span.endOffset = field.type.span.endOffset;
    if (!spanContains(span, offset)) continue;
    return Hover{"var " + field.name + ": " + annotationText(field.type, source),
                 "", span};
  }
  std::string out = iface.isPublic() ? "public " : "";
  out += "interface " + iface.getName() +
         renderTypeParameters(iface.getTypeParameters());
  return Hover{out, "", iface.getLocation()};
}

std::optional<Hover> hoverEnum(const EnumDefinitionAST& enumDef, int offset,
                               const std::string& source) {
  for (const auto& variant : enumDef.getVariants()) {
    if (!spanContains(variant.location, offset)) continue;
    std::string out = enumDef.getName() + "." + variant.name;
    for (size_t i = 0; i < variant.payloadTypes.size(); ++i) {
      out += i == 0 ? "(" : ", ";
      out += annotationText(variant.payloadTypes[i], source);
    }
    if (variant.hasPayload()) out += ")";
    return Hover{out, "", variant.location};
  }
  std::string out = enumDef.isPublic() ? "public " : "";
  out += "enum " + enumDef.getName() +
         renderTypeParameters(enumDef.getTypeParameters());
  return Hover{out, "", enumDef.getLocation()};
}

std::optional<Hover> hoverNode(const Target& target, int offset,
                               const std::string& source) {
  const ExprAST& node = target.node();
  const Bindings& bindings = target.bindings;
  const Position& range = node.getLocation();
  sun::TypePtr type = node.getResolvedType();

  // `prefix` followed by the node's own type, when it has one
  auto typed = [&](const std::string& prefix) -> std::optional<Hover> {
    if (!type) return std::nullopt;
    return Hover{prefix + renderType(*type, bindings), "", range};
  };

  switch (node.getType()) {
    case ASTNodeType::VARIABLE_REFERENCE:
      return typed(static_cast<const VariableReferenceAST&>(node).getName() +
                   ": ");
    case ASTNodeType::THIS:
      return typed("this: ");
    case ASTNodeType::MEMBER_ACCESS:
      return typed(static_cast<const MemberAccessAST&>(node).getMemberName() +
                   ": ");

    case ASTNodeType::VARIABLE_CREATION: {
      const auto& decl = static_cast<const VariableCreationAST&>(node);
      std::string prefix =
          std::string(decl.isConst() ? "const " : "var ") + decl.getName() +
          ": ";
      if (type) return Hover{prefix + renderType(*type, bindings), "", range};
      if (decl.hasTypeAnnotation()) {
        return Hover{prefix + annotationText(*decl.getTypeAnnotation(), source),
                     "", range};
      }
      return std::nullopt;
    }

    case ASTNodeType::REFERENCE_CREATION: {
      const auto& ref = static_cast<const ReferenceCreationAST&>(node);
      if (!type) return std::nullopt;
      const sun::Type* referent = type.get();
      if (type->getKind() == sun::Type::Kind::Reference) {
        referent = static_cast<const sun::ReferenceType&>(*type)
                       .getReferencedType()
                       .get();
      }
      if (!referent) return std::nullopt;
      std::string prefix = ref.isMutable() ? "ref " : "const ref ";
      return Hover{
          prefix + ref.getName() + ": " + renderType(*referent, bindings), "",
          range};
    }

    case ASTNodeType::FOR_IN_LOOP: {
      const auto& loop = static_cast<const ForInExprAST&>(node);
      std::string prefix =
          std::string(loop.isConst() ? "const " : "var ") + loop.getLoopVar() +
          ": ";
      if (loop.hasResolvedLoopVarType()) {
        return Hover{
            prefix + renderType(*loop.getResolvedLoopVarType(), bindings), "",
            range};
      }
      return Hover{prefix + annotationText(loop.getLoopVarType(), source), "",
                   range};
    }

    case ASTNodeType::FUNCTION: {
      const auto& fn = static_cast<const FunctionAST&>(node);
      return Hover{renderPrototype(fn.getProto(), "function",
                                   fn.getProto().getName(), fn.isPublic(),
                                   source, bindings),
                   "", range};
    }
    case ASTNodeType::LAMBDA: {
      const auto& lambda = static_cast<const LambdaAST&>(node);
      return Hover{renderPrototype(lambda.getProto(), "lambda", "", false,
                                   source, bindings),
                   "", range};
    }

    case ASTNodeType::CALL: {
      if (!type) return std::nullopt;
      const auto& call = static_cast<const CallExprAST&>(node);
      std::string callee = sliceSpan(source, call.getCallee()->getLocation());
      if (callee.empty()) callee = call.getCallee()->toString();
      return Hover{callee + "(...): " + renderType(*type, bindings), "",
                   range};
    }
    case ASTNodeType::GENERIC_CALL: {
      if (!type) return std::nullopt;
      const auto& call = static_cast<const GenericCallAST&>(node);
      std::string callee = call.getFunctionName();
      if (call.hasResolvedTypeArgs()) {
        const auto& typeArgs = call.getResolvedTypeArgs();
        for (size_t i = 0; i < typeArgs.size(); ++i) {
          callee += i == 0 ? "<" : ", ";
          callee += typeArgs[i] ? renderType(*typeArgs[i], bindings) : "?";
        }
        if (!typeArgs.empty()) callee += ">";
      }
      return Hover{callee + "(...): " + renderType(*type, bindings), "",
                   range};
    }

    case ASTNodeType::CLASS_DEFINITION:
      return hoverClass(static_cast<const ClassDefinitionAST&>(node), offset,
                        source, bindings);
    case ASTNodeType::INTERFACE_DEFINITION:
      return hoverInterface(static_cast<const InterfaceDefinitionAST&>(node),
                            offset, source);
    case ASTNodeType::ENUM_DEFINITION:
      return hoverEnum(static_cast<const EnumDefinitionAST&>(node), offset,
                       source);

    case ASTNodeType::STRUCT_LITERAL: {
      const auto& literal = static_cast<const StructLiteralAST&>(node);
      for (const auto& field : literal.getFields()) {
        if (!spanContains(field.location, offset)) continue;
        sun::TypePtr fieldType =
            field.value ? field.value->getResolvedType() : nullptr;
        if (!fieldType) return std::nullopt;
        return Hover{field.name + ": " + renderType(*fieldType, bindings), "",
                     field.location};
      }
      return typed("");
    }

    case ASTNodeType::MATCH: {
      const auto& match = static_cast<const MatchExprAST&>(node);
      for (const auto& arm : match.getArms()) {
        for (const auto& binding : arm.bindings) {
          if (binding.isWildcard || !binding.resolvedType) continue;
          if (!spanContains(binding.location, offset)) continue;
          return Hover{binding.name + ": " +
                           renderType(*binding.resolvedType, bindings),
                       "", binding.location};
        }
      }
      return std::nullopt;
    }

    case ASTNodeType::DECLARE_TYPE: {
      const auto& decl = static_cast<const DeclareTypeAST&>(node);
      if (!decl.hasResolvedDeclaredType()) return std::nullopt;
      std::string out = "declare ";
      if (decl.hasAlias()) out += decl.getAliasName() + " = ";
      out += renderType(*decl.getResolvedDeclaredType(), bindings);
      return Hover{out, "", range};
    }

    // Expressions: show the result type alone
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::BOOL_LITERAL:
    case ASTNodeType::NULL_LITERAL:
    case ASTNodeType::ARRAY_LITERAL:
    case ASTNodeType::BINARY:
    case ASTNodeType::UNARY:
    case ASTNodeType::TERNARY:
    case ASTNodeType::INDEX:
    case ASTNodeType::ARRAY_INDEX:
      return typed("");

    // Statements and everything else carry nothing worth showing
    default:
      return std::nullopt;
  }
}

// ---------------------------------------------------------------------------
// Finding the declaration behind a symbol
// ---------------------------------------------------------------------------

bool isDefinition(ASTNodeType kind) {
  switch (kind) {
    case ASTNodeType::FUNCTION:
    case ASTNodeType::LAMBDA:
    case ASTNodeType::CLASS_DEFINITION:
    case ASTNodeType::INTERFACE_DEFINITION:
    case ASTNodeType::ENUM_DEFINITION:
    case ASTNodeType::VARIABLE_CREATION:
    case ASTNodeType::REFERENCE_CREATION:
    case ASTNodeType::FOR_IN_LOOP:
    case ASTNodeType::DECLARE_TYPE:
      return true;
    default:
      return false;
  }
}

// Name a module-level declaration is known by, or empty for other nodes
std::string declarationName(const ExprAST& node) {
  switch (node.getType()) {
    case ASTNodeType::FUNCTION:
      return static_cast<const FunctionAST&>(node).getProto().getName();
    case ASTNodeType::CLASS_DEFINITION:
      return static_cast<const ClassDefinitionAST&>(node).getName();
    case ASTNodeType::INTERFACE_DEFINITION:
      return static_cast<const InterfaceDefinitionAST&>(node).getName();
    case ASTNodeType::ENUM_DEFINITION:
      return static_cast<const EnumDefinitionAST&>(node).getName();
    case ASTNodeType::VARIABLE_CREATION:
      return static_cast<const VariableCreationAST&>(node).getName();
    case ASTNodeType::DECLARE_TYPE: {
      const auto& decl = static_cast<const DeclareTypeAST&>(node);
      return decl.hasAlias() ? decl.getAliasName() : "";
    }
    default:
      return "";
  }
}

// Qualified name the analyzer gave a declaration, or empty
sun::QualifiedName declarationQualifiedName(const ExprAST& node) {
  switch (node.getType()) {
    case ASTNodeType::FUNCTION:
      return static_cast<const FunctionAST&>(node).getProto().getQualifiedName();
    case ASTNodeType::CLASS_DEFINITION:
      return static_cast<const ClassDefinitionAST&>(node).getQualifiedName();
    case ASTNodeType::INTERFACE_DEFINITION:
      return static_cast<const InterfaceDefinitionAST&>(node)
          .getQualifiedName();
    case ASTNodeType::VARIABLE_CREATION:
      return static_cast<const VariableCreationAST&>(node).getQualifiedName();
    default:
      return {};
  }
}

// Walks module-level declarations (through modules and moon stubs)
void forEachDeclaration(const ExprAST& node,
                        const std::function<bool(const ExprAST&)>& fn) {
  switch (node.getType()) {
    case ASTNodeType::BLOCK:
    case ASTNodeType::MODULE:
    case ASTNodeType::MOON_SCOPE:
      forEachChild(node, [&](const ExprAST& child) {
        forEachDeclaration(child, fn);
      });
      break;
    default:
      fn(node);
      break;
  }
}

// Module-level declaration with this name; a qualified-name match wins over
// a plain name match when the reference was resolved by the analyzer
const ExprAST* findDeclaration(const BlockExprAST& program,
                               const std::string& name,
                               const sun::QualifiedName& qualified) {
  const ExprAST* byName = nullptr;
  const ExprAST* byQualifiedName = nullptr;
  forEachDeclaration(program, [&](const ExprAST& decl) {
    if (declarationName(decl) != name) return true;
    if (!byName) byName = &decl;
    if (!qualified.empty() && declarationQualifiedName(decl) == qualified) {
      byQualifiedName = &decl;
    }
    return true;
  });
  return byQualifiedName ? byQualifiedName : byName;
}

const sun::Type* stripReference(const sun::Type* type) {
  while (type && type->getKind() == sun::Type::Kind::Reference) {
    type = static_cast<const sun::ReferenceType*>(type)
               ->getReferencedType()
               .get();
  }
  return type;
}

// Definition node (class, interface or enum) behind a type
const ExprAST* findTypeDefinition(const BlockExprAST& program,
                                  const sun::Type& type) {
  std::string name;
  sun::QualifiedName qualified;
  switch (type.getKind()) {
    case sun::Type::Kind::Class: {
      const auto& cls = static_cast<const sun::ClassType&>(type);
      name = cls.isSpecialized() && !cls.getBaseGenericName().empty()
                 ? cls.getBaseGenericName()
                 : cls.getBaseName();
      qualified = cls.isSpecialized() ? cls.getGenericQualifiedName()
                                      : cls.getQualifiedName();
      break;
    }
    case sun::Type::Kind::Interface: {
      const auto& iface = static_cast<const sun::InterfaceType&>(type);
      name = iface.isSpecialized() && !iface.getBaseGenericName().empty()
                 ? iface.getBaseGenericName()
                 : iface.getBaseName();
      qualified = iface.getQualifiedName();
      break;
    }
    case sun::Type::Kind::Enum: {
      const auto& enumType = static_cast<const sun::EnumType&>(type);
      name = enumType.isGenericSpecialization() ? enumType.getGenericBase()
                                                : enumType.getBaseName();
      qualified = enumType.getQualifiedName();
      break;
    }
    default:
      return nullptr;
  }
  // Names from the type registry may carry a scope path; keep the last part
  size_t dot = name.rfind('.');
  if (dot != std::string::npos) name = name.substr(dot + 1);
  return findDeclaration(program, name, qualified);
}

// Location of a member (method, field or variant) inside a definition
std::optional<Position> findMember(const ExprAST& definition,
                                   const std::string& member) {
  switch (definition.getType()) {
    case ASTNodeType::CLASS_DEFINITION: {
      const auto& cls = static_cast<const ClassDefinitionAST&>(definition);
      for (const auto& method : cls.getMethods()) {
        if (method.function && method.function->getProto().getName() == member)
          return method.function->getLocation();
      }
      for (const auto& field : cls.getFields()) {
        if (field.name == member) return field.location;
      }
      return std::nullopt;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      const auto& iface =
          static_cast<const InterfaceDefinitionAST&>(definition);
      for (const auto& method : iface.getMethods()) {
        if (method.function && method.function->getProto().getName() == member)
          return method.function->getLocation();
      }
      for (const auto& field : iface.getFields()) {
        if (field.name == member) return field.location;
      }
      return std::nullopt;
    }
    case ASTNodeType::ENUM_DEFINITION: {
      const auto& enumDef = static_cast<const EnumDefinitionAST&>(definition);
      for (const auto& variant : enumDef.getVariants()) {
        if (variant.name == member) return variant.location;
      }
      return std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

// Declaration of a local name visible at `node`: the closest earlier
// `var`/`const`/`ref` in an enclosing block, or an enclosing loop variable
std::optional<Position> findLocalDeclaration(
    const std::vector<const ExprAST*>& chain, const ExprAST& node,
    const std::string& name) {
  int offset = node.getLocation().offset;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const ExprAST& ancestor = **it;
    if (ancestor.getType() == ASTNodeType::BLOCK) {
      const ExprAST* latest = nullptr;
      for (const auto& stmt : static_cast<const BlockExprAST&>(ancestor)
                                  .getBody()) {
        if (!stmt || stmt->getLocation().offset >= offset) continue;
        if (stmt->getType() == ASTNodeType::VARIABLE_CREATION &&
            static_cast<const VariableCreationAST&>(*stmt).getName() == name) {
          latest = stmt.get();
        } else if (stmt->getType() == ASTNodeType::REFERENCE_CREATION &&
                   static_cast<const ReferenceCreationAST&>(*stmt).getName() ==
                       name) {
          latest = stmt.get();
        }
      }
      if (latest) return latest->getLocation();
    } else if (ancestor.getType() == ASTNodeType::FOR_IN_LOOP) {
      if (static_cast<const ForInExprAST&>(ancestor).getLoopVar() == name) {
        return ancestor.getLocation();
      }
    } else if (ancestor.getType() == ASTNodeType::FUNCTION ||
               ancestor.getType() == ASTNodeType::LAMBDA) {
      // Parameters have no declaration of their own to document
      const PrototypeAST& proto =
          ancestor.getType() == ASTNodeType::FUNCTION
              ? static_cast<const FunctionAST&>(ancestor).getProto()
              : static_cast<const LambdaAST&>(ancestor).getProto();
      for (const auto& arg : proto.getArgs()) {
        if (arg.first == name) return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

// Where the symbol under `node` was declared, or nothing
std::optional<Position> findDeclarationOf(
    const BlockExprAST& program, const std::vector<const ExprAST*>& chain,
    const ExprAST& node) {
  switch (node.getType()) {
    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& ref = static_cast<const VariableReferenceAST&>(node);
      if (auto local = findLocalDeclaration(chain, node, ref.getName())) {
        return local;
      }
      sun::QualifiedName qualified;
      if (ref.hasQualifiedName()) qualified = ref.getQualifiedName();
      if (const ExprAST* decl =
              findDeclaration(program, ref.getName(), qualified)) {
        return decl->getLocation();
      }
      return std::nullopt;
    }
    case ASTNodeType::GENERIC_CALL: {
      const auto& call = static_cast<const GenericCallAST&>(node);
      if (const ExprAST* decl =
              findDeclaration(program, call.getFunctionName(), {})) {
        return decl->getLocation();
      }
      return std::nullopt;
    }
    case ASTNodeType::CALL:
      return findDeclarationOf(
          program, chain, *static_cast<const CallExprAST&>(node).getCallee());
    case ASTNodeType::THIS: {
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if ((*it)->getType() == ASTNodeType::CLASS_DEFINITION) {
          return (*it)->getLocation();
        }
      }
      return std::nullopt;
    }
    case ASTNodeType::MEMBER_ACCESS: {
      const auto& access = static_cast<const MemberAccessAST&>(node);
      const ExprAST* object = access.getObject();
      if (!object) return std::nullopt;
      const sun::Type* objectType =
          stripReference(object->getResolvedType().get());
      if (!objectType) return std::nullopt;
      if (objectType->getKind() == sun::Type::Kind::Module) {
        if (const ExprAST* decl =
                findDeclaration(program, access.getMemberName(), {})) {
          return decl->getLocation();
        }
        return std::nullopt;
      }
      const ExprAST* definition = findTypeDefinition(program, *objectType);
      if (!definition) return std::nullopt;
      return findMember(*definition, access.getMemberName());
    }
    default:
      return std::nullopt;
  }
}

// ---------------------------------------------------------------------------
// Doc comments
// ---------------------------------------------------------------------------

std::string trim(const std::string& text) {
  size_t start = text.find_first_not_of(" \t\r");
  if (start == std::string::npos) return "";
  size_t end = text.find_last_not_of(" \t\r");
  return text.substr(start, end - start + 1);
}

bool startsWith(const std::string& text, const std::string& prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// One comment line with its delimiters removed
std::string stripCommentLine(std::string line) {
  line = trim(line);
  if (startsWith(line, "///")) line = line.substr(3);
  else if (startsWith(line, "//")) line = line.substr(2);
  else if (startsWith(line, "/**")) line = line.substr(3);
  else if (startsWith(line, "/*")) line = line.substr(2);
  else if (startsWith(line, "*")) line = line.substr(1);
  if (endsWith(line, "*/")) line = line.substr(0, line.size() - 2);
  return trim(line);
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

// The comment block written directly above a line (1-based): consecutive
// `//` lines, or one `/* */` block. A blank line breaks the attachment.
std::string commentAbove(const std::string& source, int line) {
  std::vector<std::string> lines = splitLines(source);
  int index = line - 2;  // line above, as a 0-based index
  if (index < 0 || index >= static_cast<int>(lines.size())) return "";

  std::vector<std::string> collected;
  std::string above = trim(lines[index]);
  if (startsWith(above, "//")) {
    while (index >= 0 && startsWith(trim(lines[index]), "//")) {
      collected.push_back(stripCommentLine(lines[index]));
      --index;
    }
  } else if (endsWith(above, "*/")) {
    int start = index;
    while (start >= 0 && trim(lines[start]).find("/*") == std::string::npos) {
      --start;
    }
    if (start < 0) return "";
    for (int i = index; i >= start; --i) {
      collected.push_back(stripCommentLine(lines[i]));
    }
  } else {
    return "";
  }

  std::string out;
  for (auto it = collected.rbegin(); it != collected.rend(); ++it) {
    if (it->empty() && out.empty()) continue;  // leading blank comment line
    if (!out.empty()) out += "\n";
    out += *it;
  }
  return trim(out);
}

// Text of the file a declaration lives in: the document itself, or a file
// registered during compilation (another file of the same manifest)
std::string sourceFor(const Position& declaration,
                      const std::string& documentPath,
                      const std::string& documentSource) {
  if (!declaration.filePath) return documentSource;
  const std::string& path = *declaration.filePath;
  if (path == documentPath || normalizePath(path) == documentPath) {
    return documentSource;
  }
  if (auto registered = SourceManager::instance().getSource(path)) {
    return *registered;
  }
  if (auto registered =
          SourceManager::instance().getSource(normalizePath(path))) {
    return *registered;
  }
  return "";
}

}  // namespace

const ExprAST* findInnermostNodeAt(const BlockExprAST& program,
                                   const std::string& filePath,
                                   int byteOffset) {
  NodeFinder finder(normalizePath(filePath), byteOffset);
  finder.visit(program);
  return finder.chain().empty() ? nullptr : finder.chain().back();
}

std::optional<Hover> computeHover(const BlockExprAST& program,
                                  const std::string& filePath,
                                  const std::string& source, int byteOffset) {
  std::string documentPath = normalizePath(filePath);
  std::optional<Target> target = locate(program, documentPath, byteOffset);
  if (!target) return std::nullopt;

  std::optional<Hover> hover = hoverNode(*target, byteOffset, source);
  if (!hover) return hover;

  // A definition documents itself (the range already points at a field or
  // variant when one was hit); anything else is documented by what it names
  std::optional<Position> declaration;
  if (isDefinition(target->node().getType()) ||
      target->node().getType() == ASTNodeType::ENUM_DEFINITION) {
    declaration = hover->range;
    if (!declaration->filePath) {
      declaration->filePath = target->node().getLocation().filePath;
    }
  } else {
    declaration = findDeclarationOf(program, target->chain, target->node());
  }
  if (declaration) {
    hover->documentation =
        commentAbove(sourceFor(*declaration, documentPath, source),
                     declaration->line);
  }
  return hover;
}

}  // namespace sun::lsp
