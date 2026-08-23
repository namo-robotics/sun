// references.cpp — Find references for the language server
//
// The symbol under the cursor is resolved to its declaration, then every
// name-bearing node in the program is resolved the same way; the ones that
// land on the same declaration are the references.

#include "lsp/references.h"

#include <algorithm>
#include <functional>
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

// Identity of a declaration: where it is. Specialization clones keep the
// template's spans, so a declaration reached through a clone and through
// the template compare equal. Parameters share their function's span and
// are told apart by name.
struct DeclarationKey {
  std::string file;
  int offset = 0;
  int end = -1;
  std::string parameter;
  bool operator==(const DeclarationKey&) const = default;
};

// Gathers the matching ranges, one per span, with the text of each file
class Collector {
 public:
  Collector(std::string documentPath, const std::string& source)
      : documentPath_(std::move(documentPath)), source_(source) {}

  // A location without a file belongs to `file`
  void setTarget(const Declaration& declaration, const std::string& file) {
    target_ = keyOf(declaration, file);
  }

  bool matches(const Declaration& declaration, const std::string& file) {
    return keyOf(declaration, file) == target_;
  }

  // Text of a file, or null when it cannot be read
  const std::string* text(const std::string& file) {
    auto cached = texts_.find(file);
    if (cached == texts_.end()) {
      Position location;
      location.filePath = file;
      cached = texts_.emplace(file, textOf(location, documentPath_, source_))
                   .first;
    }
    return cached->second ? &*cached->second : nullptr;
  }

  void add(const std::string& file, int offset, int end) {
    if (end <= offset) return;
    ranges_.emplace(normalize(file), offset, end);
  }

  std::vector<SymbolLocation> results() {
    std::vector<SymbolLocation> locations;
    for (const auto& [file, offset, end] : ranges_) {
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

  DeclarationKey keyOf(const Declaration& declaration,
                       const std::string& file) {
    DeclarationKey key;
    key.file = normalize(declaration.location.filePath.value_or(file));
    key.offset = declaration.location.offset;
    key.end = declaration.location.endOffset.value_or(-1);
    const PrototypeAST* proto =
        declaration.node ? prototypeOf(*declaration.node) : nullptr;
    if (proto && declaresParameter(*proto, declaration.name)) {
      key.parameter = declaration.name;
    }
    return key;
  }

  std::string documentPath_;
  const std::string& source_;
  DeclarationKey target_;
  std::unordered_map<std::string, std::string> normalized_;
  std::unordered_map<std::string, std::optional<std::string>> texts_;
  std::set<std::tuple<std::string, int, int>> ranges_;
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
// `implements` lists, and (when asked) the declarations themselves.
// Specialization clones carry no annotation spans, so this walks the
// templates.
class DeclaredNameFinder {
 public:
  DeclaredNameFinder(const BlockExprAST& program, bool includeDeclaration,
                     Collector& out)
      : program_(program), includeDeclaration_(includeDeclaration), out_(out) {}

  bool foundDeclaration() const { return foundDeclaration_; }

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
      if (includeDeclaration_) collectDeclarations(node, file, *text);
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
  void collectImplements(const ClassDefinitionAST& cls,
                         const std::string& file, const std::string& text) {
    if (cls.getImplementedInterfaces().empty()) return;
    const Position& span = cls.getLocation();
    if (!span.endOffset) return;
    int end = *span.endOffset;
    size_t brace = text.find('{', span.offset);
    if (brace != std::string::npos) end = std::min(end, static_cast<int>(brace));
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
      if (!out_.matches(declaration, file)) return;
      Position range = nameRangeOf(declaration, text);
      out_.add(file, range.offset, range.endOffset.value_or(range.offset));
      foundDeclaration_ = true;
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
        consider(Declaration{node.getLocation(), "", &node,
                             static_cast<const ForInExprAST&>(node).getLoopVar()});
        break;
      case ASTNodeType::MATCH: {
        for (const auto& arm : static_cast<const MatchExprAST&>(node).getArms()) {
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
  bool includeDeclaration_;
  Collector& out_;
  bool foundDeclaration_ = false;
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
                loc,
                static_cast<const GenericCallAST&>(node).getFunctionName(),
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
        considerFields(static_cast<const StructLiteralAST&>(node), file,
                       *text);
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

std::vector<SymbolLocation> computeReferences(const BlockExprAST& program,
                                              const std::string& filePath,
                                              const std::string& source,
                                              int byteOffset,
                                              bool includeDeclaration) {
  std::string documentPath = normalizePath(filePath);
  std::optional<Declaration> declaration =
      findDeclarationAt(program, documentPath, source, byteOffset);
  if (!declaration) return {};

  Collector out(documentPath, source);
  out.setTarget(*declaration, documentPath);
  DeclaredNameFinder names(program, includeDeclaration, out);
  names.visit(program, "");
  UseFinder uses(program, out);
  uses.visit(program, "");

  // A declaration outside the walked tree: one loaded from a bundle
  if (includeDeclaration && !names.foundDeclaration() &&
      declaration->location.filePath) {
    const std::string& file = *declaration->location.filePath;
    if (const std::string* text = out.text(file)) {
      Position range = nameRangeOf(*declaration, *text);
      out.add(file, range.offset, range.endOffset.value_or(range.offset));
    }
  }
  return out.results();
}

}  // namespace sun::lsp
