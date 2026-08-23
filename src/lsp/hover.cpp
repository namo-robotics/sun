// hover.cpp — Type information for the language server's hover request

#include "lsp/hover.h"

#include <utility>
#include <vector>

#include "ast.h"
#include "lsp/declarations.h"
#include "parsing/doc_comments.h"

namespace sun::lsp {

namespace {

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

// Hovers on definitions carry the comment stored on the node when the tree
// has one (declarations loaded from a bundle); otherwise the caller looks it
// up in the source.
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
    return Hover{prefix + field.name + ": " + typeText, field.doc, span};
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
  return Hover{out, cls.getDoc(), cls.getLocation()};
}

std::optional<Hover> hoverInterface(const InterfaceDefinitionAST& iface,
                                    int offset, const std::string& source) {
  for (const auto& field : iface.getFields()) {
    Position span = field.location;
    if (field.type.span.endOffset) span.endOffset = field.type.span.endOffset;
    if (!spanContains(span, offset)) continue;
    return Hover{"var " + field.name + ": " + annotationText(field.type, source),
                 field.doc, span};
  }
  std::string out = iface.isPublic() ? "public " : "";
  out += "interface " + iface.getName() +
         renderTypeParameters(iface.getTypeParameters());
  return Hover{out, iface.getDoc(), iface.getLocation()};
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
    return Hover{out, variant.doc, variant.location};
  }
  std::string out = enumDef.isPublic() ? "public " : "";
  out += "enum " + enumDef.getName() +
         renderTypeParameters(enumDef.getTypeParameters());
  return Hover{out, enumDef.getDoc(), enumDef.getLocation()};
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
      if (type) {
        return Hover{prefix + renderType(*type, bindings), decl.getDoc(),
                     range};
      }
      if (decl.hasTypeAnnotation()) {
        return Hover{prefix + annotationText(*decl.getTypeAnnotation(), source),
                     decl.getDoc(), range};
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
                   fn.getProto().getDoc(), range};
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

// Hover for a type name written in an annotation: the definition's header
// and its comment, with the annotation itself as the range
std::optional<Hover> hoverAnnotation(const ExprAST& decl,
                                     const TypeAnnotation& annotation,
                                     const std::string& source) {
  std::optional<Hover> hover;
  switch (decl.getType()) {
    case ASTNodeType::CLASS_DEFINITION:
      hover = hoverClass(static_cast<const ClassDefinitionAST&>(decl), -1,
                         source, {});
      break;
    case ASTNodeType::INTERFACE_DEFINITION:
      hover = hoverInterface(static_cast<const InterfaceDefinitionAST&>(decl),
                             -1, source);
      break;
    case ASTNodeType::ENUM_DEFINITION:
      hover = hoverEnum(static_cast<const EnumDefinitionAST&>(decl), -1,
                        source);
      break;
    case ASTNodeType::DECLARE_TYPE: {
      const auto& alias = static_cast<const DeclareTypeAST&>(decl);
      if (!alias.hasResolvedDeclaredType()) return std::nullopt;
      std::string out = "declare ";
      if (alias.hasAlias()) out += alias.getAliasName() + " = ";
      out += renderType(*alias.getResolvedDeclaredType(), {});
      hover = Hover{out, "", decl.getLocation()};
      break;
    }
    default:
      return std::nullopt;
  }
  if (hover) hover->range = annotation.span;
  return hover;
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

  // A type name written in an annotation stands for its definition
  std::optional<Declaration> declaration;
  std::optional<Hover> hover;
  if (const TypeAnnotation* annotation =
          annotationIn(target->node(), byteOffset)) {
    if (const ExprAST* decl = findAnnotatedType(program, *annotation)) {
      hover = hoverAnnotation(*decl, *annotation, source);
      if (hover) declaration = declarationOf(*decl);
    }
  }
  if (!hover) hover = hoverNode(*target, byteOffset, source);
  if (!hover) return hover;

  // A definition documents itself (the range already points at a field or
  // variant when one was hit); anything else is documented by what it names
  if (!declaration) {
    ASTNodeType kind = target->node().getType();
    if (isDefinition(kind) || kind == ASTNodeType::ENUM_DEFINITION) {
      const Position& own = target->node().getLocation();
      // A field or variant on the definition's header line has no comment
      // line of its own; the one above belongs to the definition
      bool memberOnHeaderLine =
          hover->range.offset != own.offset && hover->range.line == own.line;
      if (!memberOnHeaderLine) {
        declaration = Declaration{hover->range, ""};
        if (!declaration->location.filePath) {
          declaration->location.filePath = own.filePath;
        }
      }
    } else {
      declaration = findDeclarationOf(program, target->chain, target->node());
    }
  }

  // The comment stored on the declaration wins; otherwise read it from the
  // source the declaration was written in
  if (hover->documentation.empty() && declaration) {
    if (!declaration->doc.empty()) {
      hover->documentation = declaration->doc;
    } else {
      hover->documentation = sun::docCommentAbove(
          sourceFor(declaration->location, documentPath, source),
          declaration->location.line);
    }
  }
  return hover;
}

}  // namespace sun::lsp
