// declarations.cpp — Node lookup and declaration resolution for the language
// server

#include "lsp/declarations.h"

#include <filesystem>
#include <functional>

#include "ast.h"
#include "ast/ast_children.h"
#include "support/source_manager.h"

namespace sun::lsp {

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
// Locating the node under the cursor
// ---------------------------------------------------------------------------

bool NodeFinder::visit(const ExprAST& node) {
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

bool NodeFinder::isDocumentFile(const Position& loc) {
  if (!loc.filePath) return true;
  auto cached = fileMatches_.find(*loc.filePath);
  if (cached != fileMatches_.end()) return cached->second;
  bool matches = *loc.filePath == documentPath_ ||
                 normalizePath(*loc.filePath) == documentPath_;
  fileMatches_.emplace(*loc.filePath, matches);
  return matches;
}

namespace {

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

}  // namespace

std::optional<Target> locate(const BlockExprAST& program,
                             const std::string& documentPath, int offset) {
  NodeFinder finder(documentPath, offset);
  finder.visit(program);
  if (finder.chain().empty()) return std::nullopt;
  Target target{finder.chain(), {}};

  // The same offset is looked up inside the first specialization and its
  // concrete types are read back through the bindings. A cursor on the
  // template's own header stays on the template, which still carries the
  // annotations as written.
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
    case ASTNodeType::REFERENCE_CREATION:
      return static_cast<const ReferenceCreationAST&>(node).getName();
    case ASTNodeType::DECLARE_TYPE: {
      const auto& decl = static_cast<const DeclareTypeAST&>(node);
      return decl.hasAlias() ? decl.getAliasName() : "";
    }
    default:
      return "";
  }
}

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

namespace {

// Comment stored on a declaration node (see doc_comments.h)
std::string storedDoc(const ExprAST& node) {
  switch (node.getType()) {
    case ASTNodeType::FUNCTION:
      return static_cast<const FunctionAST&>(node).getProto().getDoc();
    case ASTNodeType::CLASS_DEFINITION:
      return static_cast<const ClassDefinitionAST&>(node).getDoc();
    case ASTNodeType::INTERFACE_DEFINITION:
      return static_cast<const InterfaceDefinitionAST&>(node).getDoc();
    case ASTNodeType::ENUM_DEFINITION:
      return static_cast<const EnumDefinitionAST&>(node).getDoc();
    case ASTNodeType::VARIABLE_CREATION:
      return static_cast<const VariableCreationAST&>(node).getDoc();
    default:
      return "";
  }
}

// Walks module-level declarations (through modules and moon stubs)
void forEachDeclaration(const ExprAST& node,
                        const std::function<void(const ExprAST&)>& fn) {
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

// Module-level declaration whose analyzer-given name mangles to `mangled`,
// falling back to the first one called `name`
const ExprAST* findDeclarationByMangledName(const BlockExprAST& program,
                                            const std::string& name,
                                            const std::string& mangled) {
  const ExprAST* byName = nullptr;
  const ExprAST* byMangledName = nullptr;
  forEachDeclaration(program, [&](const ExprAST& decl) {
    if (declarationName(decl) != name) return;
    if (!byName) byName = &decl;
    if (!mangled.empty() && !byMangledName) {
      sun::QualifiedName qualified = declarationQualifiedName(decl);
      if (!qualified.empty() && qualified.mangled() == mangled) {
        byMangledName = &decl;
      }
    }
  });
  return byMangledName ? byMangledName : byName;
}

}  // namespace

Declaration declarationOf(const ExprAST& node) {
  Declaration declaration{node.getLocation(), storedDoc(node), &node,
                          declarationName(node)};
  // A method loaded from a bundle has no span of its own, only its
  // signature's
  if (node.getType() == ASTNodeType::FUNCTION &&
      !declaration.location.endOffset) {
    const Position& proto = static_cast<const FunctionAST&>(node)
                                .getProto()
                                .getLocation();
    if (proto.endOffset) declaration.location = proto;
  }
  return declaration;
}

const ExprAST* findDeclaration(const BlockExprAST& program,
                               const std::string& name,
                               const sun::QualifiedName& qualified) {
  const ExprAST* byName = nullptr;
  const ExprAST* byQualifiedName = nullptr;
  forEachDeclaration(program, [&](const ExprAST& decl) {
    if (declarationName(decl) != name) return;
    if (!byName) byName = &decl;
    if (!qualified.empty() && declarationQualifiedName(decl) == qualified) {
      byQualifiedName = &decl;
    }
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

const ExprAST* findTypeDefinition(const BlockExprAST& program,
                                  const sun::Type& type) {
  std::string name;
  sun::QualifiedName qualified;
  switch (type.getKind()) {
    case sun::Type::Kind::Class: {
      const auto& cls = static_cast<const sun::ClassType&>(type);
      qualified = cls.isSpecialized() ? cls.getGenericQualifiedName()
                                      : cls.getQualifiedName();
      // The generic's base name may be a mangled symbol (a bundle's
      // `$hash$_sun_Vec`); the qualified name keeps the plain spelling
      if (!qualified.empty()) {
        name = qualified.baseName;
      } else if (cls.isSpecialized() && !cls.getBaseGenericName().empty()) {
        name = cls.getBaseGenericName();
      } else {
        name = cls.getBaseName();
      }
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

std::optional<Declaration> findMember(const ExprAST& definition,
                                      const std::string& member) {
  switch (definition.getType()) {
    case ASTNodeType::CLASS_DEFINITION: {
      const auto& cls = static_cast<const ClassDefinitionAST&>(definition);
      for (const auto& method : cls.getMethods()) {
        if (method.function && method.function->getProto().getName() == member)
          return declarationOf(*method.function);
      }
      for (const auto& field : cls.getFields()) {
        if (field.name == member)
          return Declaration{field.location, field.doc, nullptr, field.name};
      }
      return std::nullopt;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      const auto& iface =
          static_cast<const InterfaceDefinitionAST&>(definition);
      for (const auto& method : iface.getMethods()) {
        if (method.function && method.function->getProto().getName() == member)
          return declarationOf(*method.function);
      }
      for (const auto& field : iface.getFields()) {
        if (field.name == member)
          return Declaration{field.location, field.doc, nullptr, field.name};
      }
      return std::nullopt;
    }
    case ASTNodeType::ENUM_DEFINITION: {
      const auto& enumDef = static_cast<const EnumDefinitionAST&>(definition);
      for (const auto& variant : enumDef.getVariants()) {
        if (variant.name == member)
          return Declaration{variant.location, variant.doc, nullptr,
                             variant.name};
      }
      return std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

std::optional<Declaration> findLocalDeclaration(
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
      if (latest) return declarationOf(*latest);
    } else if (ancestor.getType() == ASTNodeType::FOR_IN_LOOP) {
      if (static_cast<const ForInExprAST&>(ancestor).getLoopVar() == name) {
        return Declaration{ancestor.getLocation(), "", &ancestor, name};
      }
    } else if (ancestor.getType() == ASTNodeType::MATCH) {
      // A payload binding is visible in its own arm's body
      for (const auto& arm : static_cast<const MatchExprAST&>(ancestor)
                                 .getArms()) {
        if (!arm.body || !spanContains(arm.body->getLocation(), offset)) {
          continue;
        }
        for (const auto& binding : arm.bindings) {
          if (!binding.isWildcard && binding.name == name) {
            return Declaration{binding.location, "", &ancestor, name};
          }
        }
      }
    } else if (ancestor.getType() == ASTNodeType::TRY_CATCH) {
      // A catch binding has no position of its own: it is declared in the
      // text between the previous block and its handler's body
      const auto& tryCatch = static_cast<const TryCatchExprAST&>(ancestor);
      const Position* previous = &tryCatch.getTryBlock().getLocation();
      for (const auto& clause : tryCatch.getCatchClauses()) {
        if (!clause.body) continue;
        const Position& body = clause.body->getLocation();
        if (clause.bindingName == name && spanContains(body, offset) &&
            previous->endOffset) {
          Position header = *previous;
          header.offset = *previous->endOffset;
          header.endOffset = body.offset;
          header.line = body.line;
          header.column = 1;
          return Declaration{header, "", &ancestor, name};
        }
        previous = &body;
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

std::optional<Declaration> findDeclarationOf(
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
        return declarationOf(*decl);
      }
      return std::nullopt;
    }
    case ASTNodeType::GENERIC_CALL: {
      const auto& call = static_cast<const GenericCallAST&>(node);
      if (const ExprAST* decl =
              findDeclaration(program, call.getFunctionName(), {})) {
        return declarationOf(*decl);
      }
      return std::nullopt;
    }
    case ASTNodeType::CALL:
      return findDeclarationOf(
          program, chain, *static_cast<const CallExprAST&>(node).getCallee());
    case ASTNodeType::THIS: {
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if ((*it)->getType() == ASTNodeType::CLASS_DEFINITION) {
          return declarationOf(**it);
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
        // `m.f`: the analyzer recorded which module's `f` was meant
        if (const ExprAST* decl = findDeclarationByMangledName(
                program, access.getMemberName(),
                access.getResolvedQualifiedName())) {
          return declarationOf(*decl);
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

// ---------------------------------------------------------------------------
// Type names written in annotations
// ---------------------------------------------------------------------------

namespace {

// Innermost annotation containing the offset (type arguments, element and
// function types nest inside the outer one), or null
const TypeAnnotation* annotationAt(const TypeAnnotation& annotation,
                                   int offset) {
  if (!spanContains(annotation.span, offset)) return nullptr;
  for (const auto& arg : annotation.typeArguments) {
    if (arg) {
      if (const TypeAnnotation* inner = annotationAt(*arg, offset)) {
        return inner;
      }
    }
  }
  for (const auto& param : annotation.paramTypes) {
    if (param) {
      if (const TypeAnnotation* inner = annotationAt(*param, offset)) {
        return inner;
      }
    }
  }
  if (annotation.elementType) {
    if (const TypeAnnotation* inner =
            annotationAt(*annotation.elementType, offset)) {
      return inner;
    }
  }
  if (annotation.returnType) {
    if (const TypeAnnotation* inner =
            annotationAt(*annotation.returnType, offset)) {
      return inner;
    }
  }
  return &annotation;
}

}  // namespace

const TypeAnnotation* annotationIn(const ExprAST& node, int offset) {
  auto check = [&](const TypeAnnotation& annotation) {
    return annotationAt(annotation, offset);
  };
  auto checkProto = [&](const PrototypeAST& proto) -> const TypeAnnotation* {
    for (const auto& arg : proto.getArgs()) {
      if (const TypeAnnotation* hit = check(arg.second)) return hit;
    }
    if (proto.hasReturnType()) {
      if (const TypeAnnotation* hit = check(*proto.getReturnType())) return hit;
    }
    if (proto.hasVariadicConstraint()) {
      if (const TypeAnnotation* hit = check(*proto.getVariadicConstraint()))
        return hit;
    }
    return nullptr;
  };

  switch (node.getType()) {
    case ASTNodeType::FUNCTION:
      return checkProto(static_cast<const FunctionAST&>(node).getProto());
    case ASTNodeType::LAMBDA:
      return checkProto(static_cast<const LambdaAST&>(node).getProto());
    case ASTNodeType::VARIABLE_CREATION: {
      const auto& decl = static_cast<const VariableCreationAST&>(node);
      return decl.hasTypeAnnotation() ? check(*decl.getTypeAnnotation())
                                      : nullptr;
    }
    case ASTNodeType::FOR_IN_LOOP:
      return check(static_cast<const ForInExprAST&>(node).getLoopVarType());
    case ASTNodeType::DECLARE_TYPE:
      return check(
          static_cast<const DeclareTypeAST&>(node).getTypeAnnotation());
    case ASTNodeType::CLASS_DEFINITION: {
      const auto& cls = static_cast<const ClassDefinitionAST&>(node);
      for (const auto& field : cls.getFields()) {
        if (const TypeAnnotation* hit = check(field.type)) return hit;
      }
      for (const auto& iface : cls.getImplementedInterfaces()) {
        for (const auto& arg : iface.typeArguments) {
          if (const TypeAnnotation* hit = check(arg)) return hit;
        }
      }
      return nullptr;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      for (const auto& field :
           static_cast<const InterfaceDefinitionAST&>(node).getFields()) {
        if (const TypeAnnotation* hit = check(field.type)) return hit;
      }
      return nullptr;
    }
    case ASTNodeType::ENUM_DEFINITION: {
      for (const auto& variant :
           static_cast<const EnumDefinitionAST&>(node).getVariants()) {
        for (const auto& payload : variant.payloadTypes) {
          if (const TypeAnnotation* hit = check(payload)) return hit;
        }
      }
      return nullptr;
    }
    case ASTNodeType::GENERIC_CALL: {
      for (const auto& arg :
           static_cast<const GenericCallAST&>(node).getTypeArguments()) {
        if (arg) {
          if (const TypeAnnotation* hit = check(*arg)) return hit;
        }
      }
      return nullptr;
    }
    case ASTNodeType::MEMBER_ACCESS: {
      for (const auto& arg :
           static_cast<const MemberAccessAST&>(node).getTypeArguments()) {
        if (arg) {
          if (const TypeAnnotation* hit = check(*arg)) return hit;
        }
      }
      return nullptr;
    }
    case ASTNodeType::TRY_CATCH: {
      for (const auto& clause :
           static_cast<const TryCatchExprAST&>(node).getCatchClauses()) {
        if (clause.bindingType) {
          if (const TypeAnnotation* hit = check(*clause.bindingType)) return hit;
        }
      }
      return nullptr;
    }
    default:
      return nullptr;
  }
}

const ExprAST* findAnnotatedType(const BlockExprAST& program,
                                 const TypeAnnotation& annotation) {
  const TypeAnnotation* named = &annotation;
  while (named->elementType &&
         (named->baseName == "ref" || named->baseName == "raw_ptr" ||
          named->baseName == "static_ptr" || named->baseName == "ptr" ||
          named->baseName == "array")) {
    named = named->elementType.get();
  }
  std::string name = named->baseName;
  size_t dot = name.rfind('.');
  if (dot != std::string::npos) name = name.substr(dot + 1);
  if (name.empty()) return nullptr;
  const ExprAST* decl = findDeclaration(program, name, {});
  if (!decl) return nullptr;
  switch (decl->getType()) {
    case ASTNodeType::CLASS_DEFINITION:
    case ASTNodeType::INTERFACE_DEFINITION:
    case ASTNodeType::ENUM_DEFINITION:
    case ASTNodeType::DECLARE_TYPE:
      return decl;
    default:
      return nullptr;
  }
}

}  // namespace sun::lsp
