// references.cpp — Find references for the language server
//
// The symbol under the cursor is resolved to its declaration, then every
// name-bearing node in the program is resolved the same way; the ones that
// land on the same declaration are the references.

#include "lsp/references.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

#include "ast.h"
#include "ast/ast_children.h"
#include "lsp/declarations.h"
#include "lsp/name_ranges.h"

namespace sun::lsp {

namespace {

// The key of a declaration known to live in `normalizedFile`
DeclarationKey keyIn(const Declaration& declaration,
                     std::string normalizedFile) {
  DeclarationKey key;
  key.file = std::move(normalizedFile);
  key.offset = declaration.location.offset;
  key.end = declaration.location.endOffset.value_or(-1);
  const PrototypeAST* proto =
      declaration.node ? prototypeOf(*declaration.node) : nullptr;
  if (proto && declaresParameter(*proto, declaration.name)) {
    key.parameter = declaration.name;
  }
  return key;
}

}  // namespace

DeclarationKey declarationKey(const Declaration& declaration,
                              const std::string& file) {
  return keyIn(declaration,
               normalizePath(declaration.location.filePath.value_or(file)));
}

namespace {

// Gathers the ranges naming any of the targets, one per span, with the
// text of each file
class Collector {
 public:
  Collector(std::string documentPath, const std::string& source)
      : documentPath_(std::move(documentPath)), source_(source) {}

  void addTarget(const Declaration& declaration, const std::string& file) {
    DeclarationKey key = keyOf(declaration, file);
    if (std::find(targets_.begin(), targets_.end(), key) == targets_.end()) {
      targets_.push_back(std::move(key));
    }
  }

  size_t targetCount() const { return targets_.size(); }

  // Index of the target a declaration is, or -1
  int indexOf(const Declaration& declaration, const std::string& file) {
    DeclarationKey key = keyOf(declaration, file);
    for (size_t i = 0; i < targets_.size(); ++i) {
      if (targets_[i] == key) return static_cast<int>(i);
    }
    return -1;
  }

  bool matches(const Declaration& declaration, const std::string& file) {
    return indexOf(declaration, file) >= 0;
  }

  // Text of a file, or null when it cannot be read
  const std::string* text(const std::string& file) {
    auto cached = texts_.find(file);
    if (cached == texts_.end()) {
      Position location;
      location.filePath = file;
      cached =
          texts_.emplace(file, textOf(location, documentPath_, source_)).first;
    }
    return cached->second ? &*cached->second : nullptr;
  }

  void add(const std::string& file, int offset, int end,
           bool isDeclaration = false) {
    if (end <= offset) return;
    auto [entry, inserted] = ranges_.emplace(
        std::make_tuple(normalize(file), offset, end), isDeclaration);
    if (!inserted && !isDeclaration) entry->second = false;
  }

  std::vector<SymbolLocation> results(bool includeDeclarations) {
    std::vector<SymbolLocation> locations;
    for (const auto& [span, isDeclaration] : ranges_) {
      if (isDeclaration && !includeDeclarations) continue;
      const auto& [file, offset, end] = span;
      const std::string* source = text(file);
      if (!source) continue;
      Position range;
      range.filePath = file;
      range.offset = offset;
      range.endOffset = end;
      locations.push_back(makeSymbolLocation(file, range, *source));
    }
    return locations;
  }

 private:
  const std::string& normalize(const std::string& path) {
    auto cached = normalized_.find(path);
    if (cached == normalized_.end()) {
      cached = normalized_.emplace(path, normalizePath(path)).first;
    }
    return cached->second;
  }

  // declarationKey with the path normalization cached
  DeclarationKey keyOf(const Declaration& declaration,
                       const std::string& file) {
    return keyIn(declaration,
                 normalize(declaration.location.filePath.value_or(file)));
  }

  std::string documentPath_;
  const std::string& source_;
  std::vector<DeclarationKey> targets_;
  std::unordered_map<std::string, std::string> normalized_;
  std::unordered_map<std::string, std::optional<std::string>> texts_;
  // Each range, flagged when it is only a declaration's name
  std::map<std::tuple<std::string, int, int>, bool> ranges_;
};

// An annotation and every one nested in it: type arguments, element,
// parameter and return types
void forEachNestedAnnotation(
    const TypeAnnotation& annotation,
    const std::function<void(const TypeAnnotation&)>& fn) {
  fn(annotation);
  for (const auto& arg : annotation.typeArguments) {
    if (arg) forEachNestedAnnotation(*arg, fn);
  }
  for (const auto& param : annotation.paramTypes) {
    if (param) forEachNestedAnnotation(*param, fn);
  }
  if (annotation.elementType) {
    forEachNestedAnnotation(*annotation.elementType, fn);
  }
  if (annotation.returnType) {
    forEachNestedAnnotation(*annotation.returnType, fn);
  }
}

// Finds names written in the tree as parsed: type names in annotations and
// `implements` lists, and the declarations themselves. Specialization
// clones carry no annotation spans, so this walks the templates.
class DeclaredNameFinder {
 public:
  DeclaredNameFinder(const BlockExprAST& program, Collector& out)
      : program_(program), out_(out) {}

  // Every target's declaration was met in the tree
  bool foundAllDeclarations() const {
    return found_.size() == out_.targetCount();
  }

  void visit(const ExprAST& node, const std::string& inheritedFile) {
    if (node.getType() == ASTNodeType::MOON_SCOPE) return;
    const Position& loc = node.getLocation();
    const std::string& file = loc.filePath ? *loc.filePath : inheritedFile;
    const std::string* text = file.empty() ? nullptr : out_.text(file);
    if (text) {
      collectAnnotations(node, file, *text);
      if (node.getType() == ASTNodeType::CLASS_DEFINITION) {
        collectImplements(static_cast<const ClassDefinitionAST&>(node), file,
                          *text);
      }
      collectDeclarations(node, file, *text);
    }
    forEachChild(node, [&](const ExprAST& child) { visit(child, file); });
  }

 private:
  void addWord(const std::string& file, const std::string& text,
               const std::string& name, int from, int to) {
    int at = findWord(text, name, from, to);
    if (at >= 0) out_.add(file, at, at + static_cast<int>(name.size()));
  }

  void collectAnnotations(const ExprAST& node, const std::string& file,
                          const std::string& text) {
    forEachAnnotation(node, [&](const TypeAnnotation& written) {
      forEachNestedAnnotation(written, [&](const TypeAnnotation& annotation) {
        if (!annotation.span.endOffset) return;
        const ExprAST* decl = findAnnotatedType(program_, annotation);
        if (!decl || !out_.matches(declarationOf(*decl), file)) return;
        addWord(file, text, declarationName(*decl), annotation.span.offset,
                *annotation.span.endOffset);
      });
    });
  }

  // Interface names follow the class name in the header, before its body
  void collectImplements(const ClassDefinitionAST& cls, const std::string& file,
                         const std::string& text) {
    if (cls.getImplementedInterfaces().empty()) return;
    const Position& span = cls.getLocation();
    if (!span.endOffset) return;
    int end = *span.endOffset;
    size_t brace = text.find('{', span.offset);
    if (brace != std::string::npos)
      end = std::min(end, static_cast<int>(brace));
    int from = findWord(text, cls.getName(), span.offset, end);
    if (from < 0) return;
    from += static_cast<int>(cls.getName().size());
    for (const auto& iface : cls.getImplementedInterfaces()) {
      const ExprAST* decl = findDeclaration(program_, iface.name, {});
      if (!decl || !out_.matches(declarationOf(*decl), file)) continue;
      addWord(file, text, iface.name, from, end);
    }
  }

  // The declarations a node makes, built as the cursor lookup builds them
  void collectDeclarations(const ExprAST& node, const std::string& file,
                           const std::string& text) {
    auto consider = [&](const Declaration& declaration) {
      int index = out_.indexOf(declaration, file);
      if (index < 0) return;
      Position range = nameRangeOf(declaration, text);
      out_.add(file, range.offset, range.endOffset.value_or(range.offset),
               true);
      found_.insert(index);
    };
    switch (node.getType()) {
      case ASTNodeType::FUNCTION:
        consider(declarationOf(node));
        [[fallthrough]];
      case ASTNodeType::LAMBDA: {
        const PrototypeAST& proto = *prototypeOf(node);
        for (const auto& arg : proto.getArgs()) {
          consider(Declaration{node.getLocation(), "", &node, arg.first});
        }
        if (proto.hasVariadicParam()) {
          consider(Declaration{node.getLocation(), "", &node,
                               *proto.getVariadicParamName()});
        }
        break;
      }
      case ASTNodeType::CLASS_DEFINITION: {
        consider(declarationOf(node));
        for (const auto& field :
             static_cast<const ClassDefinitionAST&>(node).getFields()) {
          consider(Declaration{field.location, "", nullptr, field.name});
        }
        break;
      }
      case ASTNodeType::INTERFACE_DEFINITION: {
        consider(declarationOf(node));
        for (const auto& field :
             static_cast<const InterfaceDefinitionAST&>(node).getFields()) {
          consider(Declaration{field.location, "", nullptr, field.name});
        }
        break;
      }
      case ASTNodeType::ENUM_DEFINITION: {
        consider(declarationOf(node));
        for (const auto& variant :
             static_cast<const EnumDefinitionAST&>(node).getVariants()) {
          consider(Declaration{variant.location, "", nullptr, variant.name});
        }
        break;
      }
      case ASTNodeType::VARIABLE_CREATION:
      case ASTNodeType::REFERENCE_CREATION:
      case ASTNodeType::DECLARE_TYPE:
        consider(declarationOf(node));
        break;
      case ASTNodeType::FOR_IN_LOOP:
        consider(
            Declaration{node.getLocation(), "", &node,
                        static_cast<const ForInExprAST&>(node).getLoopVar()});
        break;
      case ASTNodeType::MATCH: {
        for (const auto& arm :
             static_cast<const MatchExprAST&>(node).getArms()) {
          for (const auto& binding : arm.bindings) {
            if (!binding.isWildcard) {
              consider(Declaration{binding.location, "", &node, binding.name});
            }
          }
        }
        break;
      }
      case ASTNodeType::TRY_CATCH:
        forEachCatchBinding(
            static_cast<const TryCatchExprAST&>(node),
            [&](const CatchClause& clause, const Position& header) {
              consider(Declaration{header, "", &node, clause.bindingName});
            });
        break;
      default:
        break;
    }
  }

  const BlockExprAST& program_;
  Collector& out_;
  std::set<int> found_;  // Indices of the targets whose declaration was met
};

// Finds the uses: every expression naming a symbol, resolved the way the
// cursor is. A generic body is visited through its first specialization,
// the only analyzed copy; it keeps the template's spans.
class UseFinder {
 public:
  UseFinder(const BlockExprAST& program, Collector& out)
      : program_(program), out_(out) {}

  void visit(const ExprAST& node, const std::string& inheritedFile) {
    if (node.getType() == ASTNodeType::MOON_SCOPE) return;
    Bindings bindings;
    if (const ExprAST* specialization = firstSpecialization(node, bindings)) {
      visit(*specialization, inheritedFile);
      return;
    }
    const Position& loc = node.getLocation();
    const std::string& file = loc.filePath ? *loc.filePath : inheritedFile;

    // The chain holds the spanned ancestors from the node's own file, as the
    // cursor lookup builds it
    std::vector<const ExprAST*> saved;
    std::string savedFile;
    bool newFile = file != chainFile_;
    if (newFile) {
      saved.swap(chain_);
      savedFile = chainFile_;
      chainFile_ = file;
    }
    bool pushed = loc.endOffset.has_value();
    if (pushed) chain_.push_back(&node);

    consider(node, file);
    forEachChild(node, [&](const ExprAST& child) { visit(child, file); });

    if (pushed) chain_.pop_back();
    if (newFile) {
      chain_.swap(saved);
      chainFile_ = savedFile;
    }
  }

 private:
  // The name's own range: the span when it is exactly the name, else the
  // name at the span's start
  static std::optional<Position> identifierRange(const Position& loc,
                                                 const std::string& name,
                                                 const std::string& text) {
    if (loc.endOffset && sliceSpan(text, loc) == name) return loc;
    if (textHas(text, loc.offset, name)) {
      return rangeAt(loc, loc.offset, static_cast<int>(name.size()));
    }
    return std::nullopt;
  }

  // The member name after the object in `object.member`
  static std::optional<Position> memberRange(const ExprAST& object,
                                             const std::string& member,
                                             const Position& loc,
                                             const std::string& text) {
    if (!loc.endOffset) return std::nullopt;
    int from = object.getLocation().endOffset.value_or(loc.offset);
    int at = findWord(text, member, from, *loc.endOffset);
    if (at < 0) return std::nullopt;
    return rangeAt(loc, at, static_cast<int>(member.size()));
  }

  void considerRange(const ExprAST& node, const std::string& file,
                     const std::optional<Position>& range) {
    if (!range) return;
    std::optional<Declaration> declaration =
        resolveSymbol(program_, chain_, node);
    if (!declaration || !out_.matches(*declaration, file)) return;
    out_.add(file, range->offset, *range->endOffset);
  }

  void consider(const ExprAST& node, const std::string& file) {
    const std::string* text = file.empty() ? nullptr : out_.text(file);
    if (!text) return;
    const Position& loc = node.getLocation();
    switch (node.getType()) {
      case ASTNodeType::VARIABLE_REFERENCE:
        considerRange(
            node, file,
            identifierRange(
                loc, static_cast<const VariableReferenceAST&>(node).getName(),
                *text));
        break;
      case ASTNodeType::VARIABLE_ASSIGNMENT:
        considerRange(
            node, file,
            identifierRange(
                loc, static_cast<const VariableAssignmentAST&>(node).getName(),
                *text));
        break;
      case ASTNodeType::GENERIC_CALL:
        considerRange(
            node, file,
            identifierRange(
                loc, static_cast<const GenericCallAST&>(node).getFunctionName(),
                *text));
        break;
      case ASTNodeType::MEMBER_ACCESS: {
        const auto& access = static_cast<const MemberAccessAST&>(node);
        if (!access.getObject()) break;
        considerRange(node, file,
                      memberRange(*access.getObject(), access.getMemberName(),
                                  loc, *text));
        break;
      }
      case ASTNodeType::MEMBER_ASSIGNMENT: {
        const auto& assignment = static_cast<const MemberAssignmentAST&>(node);
        if (!assignment.getObject()) break;
        considerRange(node, file,
                      memberRange(*assignment.getObject(),
                                  assignment.getMemberName(), loc, *text));
        break;
      }
      case ASTNodeType::STRUCT_LITERAL:
        considerFields(static_cast<const StructLiteralAST&>(node), file, *text);
        break;
      default:
        break;
    }
  }

  // Field names in a struct literal name the fields of its type
  void considerFields(const StructLiteralAST& literal, const std::string& file,
                      const std::string& text) {
    const sun::Type* type = stripReference(literal.getResolvedType().get());
    if (!type) return;
    const ExprAST* definition = findTypeDefinition(program_, *type);
    if (!definition) return;
    for (const auto& field : literal.getFields()) {
      std::optional<Declaration> member = findMember(*definition, field.name);
      if (!member || !out_.matches(*member, file)) continue;
      int offset = field.location.offset;
      if (textHas(text, offset, field.name)) {
        out_.add(file, offset, offset + static_cast<int>(field.name.size()));
      }
    }
  }

  const BlockExprAST& program_;
  Collector& out_;
  std::vector<const ExprAST*> chain_;
  std::string chainFile_;
};

}  // namespace

namespace {

// Every class and interface definition in the tree, templates included
void forEachTypeDefinition(const ExprAST& node,
                           const std::function<void(const ExprAST&)>& fn) {
  if (node.getType() == ASTNodeType::MOON_SCOPE) return;
  if (node.getType() == ASTNodeType::CLASS_DEFINITION ||
      node.getType() == ASTNodeType::INTERFACE_DEFINITION) {
    fn(node);
  }
  forEachChild(node,
               [&](const ExprAST& child) { forEachTypeDefinition(child, fn); });
}

// Members of the interfaces the compiler declares itself, with no node in
// any tree
bool isBuiltinInterfaceMember(const std::string& interface,
                              const std::string& member) {
  return interface == "IError" && (member == "code" || member == "message");
}

// Builds a MemberGroup, deduplicating by declaration key
class GroupBuilder {
 public:
  GroupBuilder(const BlockExprAST& program, const std::string& documentPath)
      : program_(program), documentPath_(documentPath) {}

  MemberGroup around(const Declaration& member) {
    if (member.name.empty()) return {{member}, ""};
    const ExprAST* owner = nullptr;
    forEachTypeDefinition(program_, [&](const ExprAST& def) {
      std::optional<Declaration> own = findMember(def, member.name);
      if (own && sameDeclaration(*own, member)) owner = &def;
    });
    if (!owner) return {{member}, ""};
    name_ = member.name;
    add(member);
    if (owner->getType() == ASTNodeType::INTERFACE_DEFINITION) {
      addImplementers(*owner);
    } else {
      const auto& cls = static_cast<const ClassDefinitionAST&>(*owner);
      for (const auto& iface : cls.getImplementedInterfaces()) {
        const ExprAST* decl = findDeclaration(program_, iface.name, {});
        if (!decl) {
          if (isBuiltinInterfaceMember(iface.name, name_)) {
            group_.builtinInterface = iface.name;
          }
          continue;
        }
        std::optional<Declaration> shared = findMember(*decl, name_);
        if (!shared) continue;
        add(*shared);
        addImplementers(*decl);
      }
    }
    return std::move(group_);
  }

 private:
  bool sameDeclaration(const Declaration& a, const Declaration& b) const {
    return declarationKey(a, documentPath_) == declarationKey(b, documentPath_);
  }

  void add(const Declaration& declaration) {
    DeclarationKey key = declarationKey(declaration, documentPath_);
    if (std::find(keys_.begin(), keys_.end(), key) != keys_.end()) return;
    keys_.push_back(std::move(key));
    group_.members.push_back(declaration);
  }

  // The member in every class implementing `interface`
  void addImplementers(const ExprAST& interface) {
    forEachTypeDefinition(program_, [&](const ExprAST& def) {
      if (def.getType() != ASTNodeType::CLASS_DEFINITION) return;
      const auto& cls = static_cast<const ClassDefinitionAST&>(def);
      bool implements = false;
      for (const auto& iface : cls.getImplementedInterfaces()) {
        const ExprAST* decl = findDeclaration(program_, iface.name, {});
        if (decl &&
            sameDeclaration(declarationOf(*decl), declarationOf(interface))) {
          implements = true;
          break;
        }
      }
      if (!implements) return;
      if (std::optional<Declaration> own = findMember(def, name_)) add(*own);
    });
  }

  const BlockExprAST& program_;
  const std::string& documentPath_;
  std::string name_;
  std::vector<DeclarationKey> keys_;
  MemberGroup group_;
};

}  // namespace

MemberGroup memberGroupOf(const BlockExprAST& program,
                          const Declaration& declaration,
                          const std::string& documentPath) {
  return GroupBuilder(program, documentPath).around(declaration);
}

Occurrences findOccurrences(const BlockExprAST& program,
                            const std::string& documentPath,
                            const std::string& source,
                            const std::vector<Declaration>& targets,
                            bool includeDeclaration) {
  Collector out(documentPath, source);
  for (const Declaration& target : targets) {
    out.addTarget(target, documentPath);
  }
  DeclaredNameFinder names(program, out);
  names.visit(program, "");
  UseFinder uses(program, out);
  uses.visit(program, "");

  Occurrences occurrences;
  occurrences.allDeclared = names.foundAllDeclarations();
  occurrences.locations = out.results(includeDeclaration);
  return occurrences;
}

std::vector<SymbolLocation> computeReferences(const BlockExprAST& program,
                                              const std::string& filePath,
                                              const std::string& source,
                                              int byteOffset,
                                              bool includeDeclaration) {
  std::string documentPath = normalizePath(filePath);
  std::optional<Declaration> declaration =
      findDeclarationAt(program, documentPath, source, byteOffset);
  if (!declaration) return {};

  Occurrences occurrences = findOccurrences(
      program, documentPath, source,
      memberGroupOf(program, *declaration, documentPath).members,
      includeDeclaration);
  std::vector<SymbolLocation>& locations = occurrences.locations;

  // A declaration outside the walked tree: one loaded from a bundle
  if (includeDeclaration && !occurrences.allDeclared &&
      declaration->location.filePath) {
    const std::string& file = *declaration->location.filePath;
    Position location;
    location.filePath = file;
    std::optional<std::string> text = textOf(location, documentPath, source);
    if (text) {
      Position range = nameRangeOf(*declaration, *text);
      if (range.endOffset.value_or(range.offset) > range.offset) {
        locations.push_back(
            makeSymbolLocation(normalizePath(file), range, *text));
        std::sort(locations.begin(), locations.end(),
                  [](const SymbolLocation& a, const SymbolLocation& b) {
                    return std::tie(a.filePath, a.range.offset) <
                           std::tie(b.filePath, b.range.offset);
                  });
      }
    }
  }
  return locations;
}

}  // namespace sun::lsp
