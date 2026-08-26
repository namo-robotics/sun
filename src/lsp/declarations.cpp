// declarations.cpp — Node lookup and declaration resolution for the language
// server

#include "lsp/declarations.h"

#include <filesystem>
#include <functional>

#include "ast.h"
#include "ast/ast_children.h"
#include "lsp/name_ranges.h"
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
      return static_cast<const FunctionAST&>(node)
          .getProto()
          .getQualifiedName();
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
      forEachChild(
          node, [&](const ExprAST& child) { forEachDeclaration(child, fn); });
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
    const Position& proto =
        static_cast<const FunctionAST&>(node).getProto().getLocation();
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
    type =
        static_cast<const sun::ReferenceType*>(type)->getReferencedType().get();
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
      for (const auto& stmt :
           static_cast<const BlockExprAST&>(ancestor).getBody()) {
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
      for (const auto& arm :
           static_cast<const MatchExprAST&>(ancestor).getArms()) {
        if (!arm.body || !spanContains(arm.body->getLocation(), offset)) {
          continue;
        }
        for (const auto& binding : arm.bindings) {
          if (!binding.isWildcard && binding.name == name) {
            return Declaration{binding.location, "", &ancestor, name};
          }
        }
      }
    } else if (ancestor.getType() == ASTNodeType::FOR_LOOP) {
      // `for (var i = 0; ...)`: the loop's own variable
      const ExprAST* init = static_cast<const ForExprAST&>(ancestor).getInit();
      if (init && init->getType() == ASTNodeType::VARIABLE_CREATION &&
          init->getLocation().offset < offset &&
          static_cast<const VariableCreationAST&>(*init).getName() == name) {
        return declarationOf(*init);
      }
    } else if (ancestor.getType() == ASTNodeType::TRY_CATCH) {
      // A catch binding is visible in its own handler's body
      std::optional<Declaration> found;
      forEachCatchBinding(
          static_cast<const TryCatchExprAST&>(ancestor),
          [&](const CatchClause& clause, const Position& header) {
            if (!found && clause.bindingName == name &&
                spanContains(clause.body->getLocation(), offset)) {
              found = Declaration{header, "", &ancestor, name};
            }
          });
      if (found) return found;
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

std::optional<Declaration> findMemberDeclaration(
    const BlockExprAST& program, const ExprAST& object,
    const std::string& member, const std::string& qualifiedName) {
  const sun::Type* objectType = stripReference(object.getResolvedType().get());
  if (!objectType) {
    // A match pattern's object is never typed: `Shape.Circle(r)` names the
    // enum directly
    if (object.getType() != ASTNodeType::VARIABLE_REFERENCE) {
      return std::nullopt;
    }
    const auto& ref = static_cast<const VariableReferenceAST&>(object);
    sun::QualifiedName qualified;
    if (ref.hasQualifiedName()) qualified = ref.getQualifiedName();
    const ExprAST* definition =
        findDeclaration(program, ref.getName(), qualified);
    if (!definition) return std::nullopt;
    return findMember(*definition, member);
  }
  if (objectType->getKind() == sun::Type::Kind::Module) {
    // `m.f`: the analyzer recorded which module's `f` was meant
    if (const ExprAST* decl =
            findDeclarationByMangledName(program, member, qualifiedName)) {
      return declarationOf(*decl);
    }
    return std::nullopt;
  }
  const ExprAST* definition = findTypeDefinition(program, *objectType);
  if (!definition) return std::nullopt;
  return findMember(*definition, member);
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
      if (!access.getObject()) return std::nullopt;
      return findMemberDeclaration(program, *access.getObject(),
                                   access.getMemberName(),
                                   access.getQualifiedName().mangled());
    }
    case ASTNodeType::MEMBER_ASSIGNMENT: {
      const auto& assignment = static_cast<const MemberAssignmentAST&>(node);
      if (!assignment.getObject()) return std::nullopt;
      return findMemberDeclaration(program, *assignment.getObject(),
                                   assignment.getMemberName(), "");
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

void forEachAnnotation(const ExprAST& node, const AnnotationFn& fn) {
  auto visitProto = [&](const PrototypeAST& proto) {
    for (const auto& arg : proto.getArgs()) fn(arg.second);
    if (proto.hasReturnType()) fn(*proto.getReturnType());
    if (proto.hasVariadicTypeAnnotation())
      fn(proto.getVariadicTypeAnnotation());
  };

  switch (node.getType()) {
    case ASTNodeType::FUNCTION:
      visitProto(static_cast<const FunctionAST&>(node).getProto());
      break;
    case ASTNodeType::LAMBDA:
      visitProto(static_cast<const LambdaAST&>(node).getProto());
      break;
    case ASTNodeType::VARIABLE_CREATION: {
      const auto& decl = static_cast<const VariableCreationAST&>(node);
      if (decl.hasTypeAnnotation()) fn(*decl.getTypeAnnotation());
      break;
    }
    case ASTNodeType::FOR_IN_LOOP:
      fn(static_cast<const ForInExprAST&>(node).getLoopVarType());
      break;
    case ASTNodeType::DECLARE_TYPE:
      fn(static_cast<const DeclareTypeAST&>(node).getTypeAnnotation());
      break;
    case ASTNodeType::CLASS_DEFINITION: {
      const auto& cls = static_cast<const ClassDefinitionAST&>(node);
      for (const auto& field : cls.getFields()) fn(field.type);
      for (const auto& iface : cls.getImplementedInterfaces()) {
        for (const auto& arg : iface.typeArguments) fn(arg);
      }
      break;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      for (const auto& field :
           static_cast<const InterfaceDefinitionAST&>(node).getFields()) {
        fn(field.type);
      }
      break;
    }
    case ASTNodeType::ENUM_DEFINITION: {
      for (const auto& variant :
           static_cast<const EnumDefinitionAST&>(node).getVariants()) {
        for (const auto& payload : variant.payloadTypes) fn(payload);
      }
      break;
    }
    case ASTNodeType::GENERIC_CALL: {
      for (const auto& arg :
           static_cast<const GenericCallAST&>(node).getTypeArguments()) {
        if (arg) fn(*arg);
      }
      break;
    }
    case ASTNodeType::MEMBER_ACCESS: {
      for (const auto& arg :
           static_cast<const MemberAccessAST&>(node).getTypeArguments()) {
        if (arg) fn(*arg);
      }
      break;
    }
    case ASTNodeType::TRY_CATCH: {
      for (const auto& clause :
           static_cast<const TryCatchExprAST&>(node).getCatchClauses()) {
        if (clause.bindingType) fn(*clause.bindingType);
      }
      break;
    }
    default:
      break;
  }
}

const TypeAnnotation* annotationIn(const ExprAST& node, int offset) {
  const TypeAnnotation* hit = nullptr;
  forEachAnnotation(node, [&](const TypeAnnotation& annotation) {
    if (!hit) hit = annotationAt(annotation, offset);
  });
  return hit;
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

// ---------------------------------------------------------------------------
// Resolving the symbol a node names
// ---------------------------------------------------------------------------

std::optional<Declaration> findParameter(
    const std::vector<const ExprAST*>& chain, const std::string& name) {
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const PrototypeAST* proto = prototypeOf(**it);
    if (proto && declaresParameter(*proto, name)) {
      return Declaration{(*it)->getLocation(), "", *it, name};
    }
  }
  return std::nullopt;
}

void forEachCatchBinding(const TryCatchExprAST& tryCatch,
                         const CatchBindingFn& fn) {
  const Position* previous = &tryCatch.getTryBlock().getLocation();
  for (const auto& clause : tryCatch.getCatchClauses()) {
    if (!clause.body) continue;
    const Position& body = clause.body->getLocation();
    if (previous->endOffset) {
      Position header = *previous;
      header.offset = *previous->endOffset;
      header.endOffset = body.offset;
      header.line = body.line;
      header.column = 1;
      fn(clause, header);
    }
    previous = &body;
  }
}

namespace {

// True when the offset lies in a definition's header, before its body
bool inHeader(const ExprAST& node, int offset, const std::string& source) {
  size_t brace = source.find('{', node.getLocation().offset);
  return brace == std::string::npos || offset < static_cast<int>(brace);
}

// The parameter written at the offset in a function or lambda signature
std::optional<Declaration> parameterUnder(const ExprAST& owner, int offset,
                                          const std::string& source) {
  const PrototypeAST* proto = prototypeOf(owner);
  if (!proto) return std::nullopt;
  std::vector<std::string> names = proto->getArgNames();
  if (proto->hasVariadicParam()) names.push_back(proto->getVariadicParamName());
  for (const auto& name : names) {
    std::optional<Position> range = parameterRange(owner, name, source);
    if (range && spanContains(*range, offset)) {
      return Declaration{owner.getLocation(), "", &owner, name};
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<Declaration> ownDeclaration(const ExprAST& node, int offset,
                                          const std::string& source) {
  switch (node.getType()) {
    case ASTNodeType::CLASS_DEFINITION: {
      for (const auto& field :
           static_cast<const ClassDefinitionAST&>(node).getFields()) {
        if (spanContains(field.location, offset)) {
          return Declaration{field.location, "", nullptr, field.name};
        }
      }
      break;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      for (const auto& field :
           static_cast<const InterfaceDefinitionAST&>(node).getFields()) {
        if (spanContains(field.location, offset)) {
          return Declaration{field.location, "", nullptr, field.name};
        }
      }
      break;
    }
    case ASTNodeType::ENUM_DEFINITION: {
      for (const auto& variant :
           static_cast<const EnumDefinitionAST&>(node).getVariants()) {
        if (spanContains(variant.location, offset)) {
          return Declaration{variant.location, "", nullptr, variant.name};
        }
      }
      break;
    }
    case ASTNodeType::MATCH: {
      for (const auto& arm : static_cast<const MatchExprAST&>(node).getArms()) {
        for (const auto& binding : arm.bindings) {
          if (!binding.isWildcard && spanContains(binding.location, offset)) {
            return Declaration{binding.location, "", nullptr, binding.name};
          }
        }
      }
      return std::nullopt;
    }
    case ASTNodeType::TRY_CATCH: {
      std::optional<Declaration> found;
      forEachCatchBinding(
          static_cast<const TryCatchExprAST&>(node),
          [&](const CatchClause& clause, const Position& header) {
            if (found || !spanContains(header, offset)) return;
            int at = findWord(source, clause.bindingName, header.offset,
                              *header.endOffset);
            int length = static_cast<int>(clause.bindingName.size());
            if (at >= 0 && at <= offset && offset < at + length) {
              found = Declaration{header, "", &node, clause.bindingName};
            }
          });
      return found;
    }
    case ASTNodeType::VARIABLE_CREATION:
    case ASTNodeType::REFERENCE_CREATION:
    case ASTNodeType::DECLARE_TYPE:
      return declarationOf(node);
    case ASTNodeType::LAMBDA:
      return parameterUnder(node, offset, source);
    case ASTNodeType::FUNCTION:
      if (auto parameter = parameterUnder(node, offset, source)) {
        return parameter;
      }
      break;
    case ASTNodeType::FOR_IN_LOOP:
      break;
    default:
      return std::nullopt;
  }
  if (declarationName(node).empty() &&
      node.getType() != ASTNodeType::FOR_IN_LOOP) {
    return std::nullopt;
  }
  if (!inHeader(node, offset, source)) return std::nullopt;
  if (node.getType() == ASTNodeType::FOR_IN_LOOP) {
    return Declaration{node.getLocation(), "", &node,
                       static_cast<const ForInExprAST&>(node).getLoopVar()};
  }
  return declarationOf(node);
}

std::optional<Declaration> resolveSymbol(
    const BlockExprAST& program, const std::vector<const ExprAST*>& chain,
    const ExprAST& node) {
  std::string name;
  if (node.getType() == ASTNodeType::VARIABLE_REFERENCE) {
    name = static_cast<const VariableReferenceAST&>(node).getName();
  } else if (node.getType() == ASTNodeType::VARIABLE_ASSIGNMENT) {
    name = static_cast<const VariableAssignmentAST&>(node).getName();
  }
  if (name.empty()) return findDeclarationOf(program, chain, node);
  if (auto local = findLocalDeclaration(chain, node, name)) return local;
  if (auto parameter = findParameter(chain, name)) return parameter;
  if (node.getType() == ASTNodeType::VARIABLE_REFERENCE) {
    return findDeclarationOf(program, chain, node);
  }
  const ExprAST* decl = findDeclaration(program, name, {});
  if (!decl) return std::nullopt;
  return declarationOf(*decl);
}

namespace {

// The field a struct literal names at the offset: `{ x: 1 }` names the `x`
// of the literal's type
std::optional<Declaration> findLiteralField(const BlockExprAST& program,
                                            const StructLiteralAST& literal,
                                            int offset,
                                            const std::string& source) {
  const sun::Type* type = stripReference(literal.getResolvedType().get());
  if (!type) return std::nullopt;
  const ExprAST* definition = findTypeDefinition(program, *type);
  if (!definition) return std::nullopt;
  for (const auto& field : literal.getFields()) {
    int start = field.location.offset;
    int end = start + static_cast<int>(field.name.size());
    if (offset >= start && offset < end && textHas(source, start, field.name)) {
      return findMember(*definition, field.name);
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<Declaration> declarationUnder(const BlockExprAST& program,
                                            const Target& target, int offset,
                                            const std::string& source) {
  const ExprAST& node = target.node();
  if (node.getType() == ASTNodeType::STRUCT_LITERAL) {
    return findLiteralField(program, static_cast<const StructLiteralAST&>(node),
                            offset, source);
  }
  if (auto own = ownDeclaration(node, offset, source)) return own;
  return resolveSymbol(program, target.chain, node);
}

std::optional<Declaration> findDeclarationAt(const BlockExprAST& program,
                                             const std::string& documentPath,
                                             const std::string& source,
                                             int byteOffset) {
  // A type name written in an annotation stands for its definition. This is
  // checked on the tree as parsed: specialization clones carry no annotation
  // spans, so the redirected lookup below cannot see them.
  NodeFinder finder(documentPath, byteOffset);
  finder.visit(program);
  if (finder.chain().empty()) return std::nullopt;
  if (const TypeAnnotation* annotation =
          annotationIn(*finder.chain().back(), byteOffset)) {
    if (const ExprAST* decl = findAnnotatedType(program, *annotation)) {
      return declarationOf(*decl);
    }
  }

  std::optional<Target> target = locate(program, documentPath, byteOffset);
  if (!target) return std::nullopt;
  return declarationUnder(program, *target, byteOffset, source);
}

}  // namespace sun::lsp
