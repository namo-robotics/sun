// formatter.cpp — Canonical source formatter (sun fmt)
//
// Prints the lossless pre-lowering parse tree (ParenExprAST and
// InterpolatedStringAST intact; LoweringPass is never run). Literals and
// types are sliced verbatim from the source by span; comments come from the
// parser's side table and are interleaved by offset.

#include "parsing/formatter.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "parsing/parser.h"
#include "support/error.h"

namespace sun {
namespace {

constexpr int kIndentWidth = 2;

class Formatter {
 public:
  Formatter(const std::string& src, const std::map<int, Comment>& comments)
      : src_(src), comments_(comments), next_(comments.begin()) {}

  std::string format(const BlockExprAST& program) {
    printStmts(program.getBody());
    flushCommentsBefore(static_cast<int>(src_.size()));
    // Exactly one trailing newline
    while (!out_.empty() && out_.back() == '\n') out_.pop_back();
    if (!out_.empty()) out_ += '\n';
    return out_;
  }

 private:
  const std::string& src_;
  const std::map<int, Comment>& comments_;
  std::map<int, Comment>::const_iterator next_;
  std::string out_;
  int indent_ = 0;
  int lastLine_ = -1;  // Source end line of the last emitted element; -1
                       // suppresses the blank-line check (start of scope)

  // --- small helpers -----------------------------------------------------

  static int endLineOf(const Position& p) { return p.endLine.value_or(p.line); }

  static bool isMultiLine(const Position& p) {
    return p.endLine.has_value() && *p.endLine > p.line;
  }

  std::string slice(const Position& p) const {
    if (!p.endOffset.has_value()) return "";
    return src_.substr(p.offset, *p.endOffset - p.offset);
  }

  void writeIndent() { out_.append(indent_ * kIndentWidth, ' '); }

  // Preserve at most one blank line from the source
  void blankGap(int nextStartLine) {
    if (lastLine_ >= 0 && nextStartLine - lastLine_ >= 2) out_ += '\n';
  }

  bool hasCommentBefore(int offset) const {
    return next_ != comments_.end() && next_->first < offset;
  }

  // Own-line emission of every comment starting before `offset`
  void flushCommentsBefore(int offset) {
    while (next_ != comments_.end() && next_->first < offset) {
      const Comment& c = next_->second;
      blankGap(c.span.line);
      writeIndent();
      out_ += c.text;  // multi-line block comments keep their raw interior
      out_ += '\n';
      lastLine_ = endLineOf(c.span);
      ++next_;
    }
  }

  // Comments on the same source line as the element just printed are
  // appended as trailing comments (two spaces before, gofmt-style)
  void emitTrailingComments(int elemEndLine) {
    while (next_ != comments_.end() && !next_->second.ownLine &&
           next_->second.span.line == elemEndLine) {
      const Comment& c = next_->second;
      out_ += "  ";
      out_ += c.text;
      lastLine_ = endLineOf(c.span);
      ++next_;
    }
  }

  // --- statements ---------------------------------------------------------

  void printStmts(const std::vector<std::unique_ptr<ExprAST>>& stmts) {
    for (const auto& stmt : stmts) {
      if (!stmt || stmt->isPrecompiled()) continue;
      const Position& loc = stmt->getLocation();
      flushCommentsBefore(loc.offset);
      blankGap(loc.line);
      writeIndent();
      printStmt(*stmt);
      lastLine_ = endLineOf(loc);
      emitTrailingComments(endLineOf(loc));
      out_ += '\n';
    }
  }

  void printStmt(const ExprAST& e) {
    printVisibility(e.getVisibility());
    printExpr(e);
    if (needsSemicolon(e)) out_ += ';';
  }

  void printVisibility(sun::Visibility v) {
    if (v == sun::Visibility::Public) out_ += "public ";
  }

  // Drop a leading `public` from a verbatim slice; the modifier is re-emitted
  // by printVisibility so it is not doubled.
  static std::string stripPublic(std::string s) {
    static const std::string kw = "public";
    if (s.compare(0, kw.size(), kw) == 0 && s.size() > kw.size() &&
        std::isspace(static_cast<unsigned char>(s[kw.size()]))) {
      size_t i = kw.size();
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
      s.erase(0, i);
    }
    return s;
  }

  static bool needsSemicolon(const ExprAST& e) {
    switch (e.getType()) {
      case ASTNodeType::FUNCTION:
        return static_cast<const FunctionAST&>(e).isExtern();
      case ASTNodeType::CLASS_DEFINITION:
      case ASTNodeType::INTERFACE_DEFINITION:
      case ASTNodeType::ENUM_DEFINITION:
      case ASTNodeType::MODULE:
      case ASTNodeType::MANIFEST:
      case ASTNodeType::MOON_SCOPE:
      case ASTNodeType::IMPORT_SCOPE:
      case ASTNodeType::IF:
      case ASTNodeType::WHILE_LOOP:
      case ASTNodeType::FOR_LOOP:
      case ASTNodeType::FOR_IN_LOOP:
      case ASTNodeType::TRY_CATCH:
      case ASTNodeType::BLOCK:
        return false;
      default:
        return true;
    }
  }

  // --- blocks -------------------------------------------------------------

  // Multiline block: '{' newline, indented statements, '}'
  void printBlockML(const BlockExprAST& b) {
    const Position& loc = b.getLocation();
    int endOffset = loc.endOffset.value_or(static_cast<int>(src_.size()));
    if (b.isEmpty() && !hasCommentBefore(endOffset)) {
      out_ += "{}";
      return;
    }
    out_ += "{\n";
    ++indent_;
    int savedLast = lastLine_;
    lastLine_ = -1;  // no blank line right after '{'
    printStmts(b.getBody());
    flushCommentsBefore(endOffset);
    --indent_;
    writeIndent();
    out_ += '}';
    lastLine_ = savedLast;
  }

  // Block that may stay on one line when the source kept it on one line
  // (lambda and unsafe bodies); comments force the multiline form
  void printBlockAuto(const BlockExprAST& b) {
    const Position& loc = b.getLocation();
    int endOffset = loc.endOffset.value_or(static_cast<int>(src_.size()));
    if (!isMultiLine(loc) && !hasCommentBefore(endOffset)) {
      if (b.isEmpty()) {
        out_ += "{}";
        return;
      }
      out_ += "{ ";
      for (const auto& stmt : b.getBody()) {
        if (!stmt) continue;
        printStmt(*stmt);
        out_ += ' ';
      }
      out_ += '}';
      return;
    }
    printBlockML(b);
  }

  // --- types & prototypes ---------------------------------------------------

  void printType(const TypeAnnotation& t) {
    if (t.span.endOffset.has_value()) {
      out_ += slice(t.span);
    } else {
      out_ += t.toString();  // fallback; parsed types always carry spans
    }
  }

  // `<'a, T, U: _Numeric>`, or nothing at all when the declaration has no
  // parameters. Lifetimes come first, as they are written.
  void printTypeParams(const std::vector<TypeParameter>& typeParams,
                       const std::vector<LifetimeParameter>& lifetimes = {}) {
    if (typeParams.empty() && lifetimes.empty()) return;
    out_ += '<';
    bool first = true;
    for (const auto& lifetime : lifetimes) {
      if (!first) out_ += ", ";
      out_ += lifetime.toString();
      first = false;
    }
    for (const auto& typeParam : typeParams) {
      if (!first) out_ += ", ";
      out_ += typeParam.toString();
      first = false;
    }
    out_ += '>';
  }

  void printProtoSig(const PrototypeAST& p) {
    out_ += p.getName();
    printTypeParams(p.getTypeParameters(), p.getLifetimeParameters());
    out_ += '(';
    const auto& args = p.getArgs();
    bool first = true;
    for (const auto& [name, type] : args) {
      if (!first) out_ += ", ";
      first = false;
      out_ += name;
      out_ += ": ";
      printType(type);
    }
    if (p.hasVariadicParam()) {
      if (!first) out_ += ", ";
      out_ += p.getVariadicParamName();
      out_ += "...";
      if (p.hasVariadicTypeAnnotation()) {
        out_ += ": ";
        printType(p.getVariadicTypeAnnotation());
      }
    }
    if (p.isCVariadic()) {
      if (!first) out_ += ", ";
      out_ += "...";
    }
    out_ += ')';
    // Span-less return types are parser-synthesized (init and deinit get an
    // implicit void); only source-spelled types are printed
    if (p.getReturnType().has_value() &&
        p.getReturnType()->span.endOffset.has_value()) {
      out_ += ' ';
      printType(*p.getReturnType());
    } else if (p.getReturnType().has_value() && p.getReturnType()->canError) {
      // A throwing constructor spells only the error part:
      // init(...) throws IError
      out_ += " throws IError";
    }
  }

  // `asMethod` prints the class/interface member keyword instead of
  // 'function'; the rest of the signature is spelled the same way.
  void printFunction(const FunctionAST& f, bool asMethod = false) {
    if (f.isExtern()) {
      // Bodyless functions are `extern` or `declare` forward declarations
      if (f.isCExtern()) {
        // Preserve an explicit ABI string; it is optional in the source and
        // not stored on the AST, so recover it from the span.
        std::string s = stripPublic(slice(f.getLocation()));
        out_ += s.rfind("extern \"C\"", 0) == 0 ? "extern \"C\" function "
                                                : "extern function ";
      } else {
        out_ += "declare function ";
      }
      printProtoSig(f.getProto());
      if (f.getProto().hasLinkName()) {
        out_ += " as \"" + f.getProto().getLinkName() + '"';
      }
      return;  // ';' comes from needsSemicolon
    }
    out_ += asMethod ? "method " : "function ";
    printProtoSig(f.getProto());
    out_ += ' ';
    printBlockML(f.getBody());
  }

  // --- expression lists -----------------------------------------------------

  // Single line: (a, b, c). Multiline (when the construct spanned lines in
  // the source): one element per line, closing bracket at parent indent.
  void printExprList(const std::vector<std::unique_ptr<ExprAST>>& items,
                     char open, char close, const Position& constructSpan) {
    out_ += open;
    bool multiline = isMultiLine(constructSpan) && !items.empty();
    if (!multiline) {
      for (size_t i = 0; i < items.size(); ++i) {
        if (i) out_ += ", ";
        printExpr(*items[i]);
      }
      out_ += close;
      return;
    }
    out_ += '\n';
    ++indent_;
    for (size_t i = 0; i < items.size(); ++i) {
      const Position& loc = items[i]->getLocation();
      flushCommentsBefore(loc.offset);
      writeIndent();
      printExpr(*items[i]);
      if (i + 1 < items.size()) out_ += ',';
      lastLine_ = endLineOf(loc);
      emitTrailingComments(endLineOf(loc));
      out_ += '\n';
    }
    int endOffset =
        constructSpan.endOffset.value_or(static_cast<int>(src_.size()));
    flushCommentsBefore(endOffset - 1);
    --indent_;
    writeIndent();
    out_ += close;
  }

  // --- declarations ---------------------------------------------------------

  void printClass(const ClassDefinitionAST& c) {
    if (c.isPartial()) out_ += "partial ";
    out_ += c.classKeyword();
    out_ += ' ';
    out_ += c.getName();
    printTypeParams(c.getTypeParameters(), c.getLifetimeParameters());
    const auto& ifaces = c.getImplementedInterfaces();
    if (!ifaces.empty()) {
      out_ += " implements ";
      for (size_t i = 0; i < ifaces.size(); ++i) {
        if (i) out_ += ", ";
        out_ += ifaces[i].name;
        if (!ifaces[i].typeArguments.empty()) {
          out_ += '<';
          for (size_t j = 0; j < ifaces[i].typeArguments.size(); ++j) {
            if (j) out_ += ", ";
            printType(ifaces[i].typeArguments[j]);
          }
          out_ += '>';
        }
      }
    }
    out_ += " {\n";
    ++indent_;
    int savedLast = lastLine_;
    lastLine_ = -1;

    // Fields and methods live in separate vectors; merge by source order
    struct Member {
      int offset;
      int line;
      int endLine;
      const ClassFieldDecl* field;
      const FunctionAST* method;
    };
    std::vector<Member> members;
    for (const auto& f : c.getFields()) {
      members.push_back({f.location.offset, f.location.line,
                         endLineOf(f.location), &f, nullptr});
    }
    for (const auto& m : c.getMethods()) {
      const Position& loc = m.function->getLocation();
      members.push_back(
          {loc.offset, loc.line, endLineOf(loc), nullptr, m.function.get()});
    }
    std::sort(
        members.begin(), members.end(),
        [](const Member& a, const Member& b) { return a.offset < b.offset; });

    for (const auto& m : members) {
      flushCommentsBefore(m.offset);
      blankGap(m.line);
      writeIndent();
      if (m.field) {
        printVisibility(m.field->visibility);
        out_ += "var ";
        out_ += m.field->name;
        out_ += ": ";
        printType(m.field->type);
        out_ += ';';
      } else {
        const std::string& methodName = m.method->getProto().getName();
        if (methodName == "init" || methodName == "deinit") {
          // Constructors and destructors are written bare: init(...) { }
          printProtoSig(m.method->getProto());
          out_ += ' ';
          printBlockML(m.method->getBody());
        } else {
          printVisibility(m.method->getVisibility());
          if (m.method->getProto().isConstMethod()) out_ += "const ";
          printFunction(*m.method, /*asMethod=*/true);
          if (m.method->isExtern()) out_ += ';';
        }
      }
      lastLine_ = m.endLine;
      emitTrailingComments(m.endLine);
      out_ += '\n';
    }

    int endOffset =
        c.getLocation().endOffset.value_or(static_cast<int>(src_.size()));
    flushCommentsBefore(endOffset);
    --indent_;
    writeIndent();
    out_ += '}';
    lastLine_ = savedLast;
  }

  void printInterface(const InterfaceDefinitionAST& n) {
    out_ += "interface ";
    out_ += n.getName();
    printTypeParams(n.getTypeParameters(), n.getLifetimeParameters());
    out_ += " {\n";
    ++indent_;
    int savedLast = lastLine_;
    lastLine_ = -1;

    struct Member {
      int offset;
      int line;
      int endLine;
      const InterfaceFieldDecl* field;
      const InterfaceMethodDecl* method;
    };
    std::vector<Member> members;
    for (const auto& f : n.getFields()) {
      members.push_back({f.location.offset, f.location.line,
                         endLineOf(f.location), &f, nullptr});
    }
    for (const auto& m : n.getMethods()) {
      const Position& loc = m.function->getLocation();
      members.push_back({loc.offset, loc.line, endLineOf(loc), nullptr, &m});
    }
    std::sort(
        members.begin(), members.end(),
        [](const Member& a, const Member& b) { return a.offset < b.offset; });

    for (const auto& m : members) {
      flushCommentsBefore(m.offset);
      blankGap(m.line);
      writeIndent();
      if (m.field) {
        printVisibility(m.field->visibility);
        out_ += "var ";
        out_ += m.field->name;
        out_ += ": ";
        printType(m.field->type);
        out_ += ';';
      } else if (!m.method->hasDefaultImpl) {
        printVisibility(m.method->visibility());
        if (m.method->isConst) out_ += "const ";
        // Signature-only method (the parser synthesizes an empty body)
        out_ += "method ";
        printProtoSig(m.method->function->getProto());
        out_ += ';';
      } else {
        printVisibility(m.method->visibility());
        if (m.method->isConst) out_ += "const ";
        printFunction(*m.method->function, /*asMethod=*/true);
      }
      lastLine_ = m.endLine;
      emitTrailingComments(m.endLine);
      out_ += '\n';
    }

    int endOffset =
        n.getLocation().endOffset.value_or(static_cast<int>(src_.size()));
    flushCommentsBefore(endOffset);
    --indent_;
    writeIndent();
    out_ += '}';
    lastLine_ = savedLast;
  }

  void printVariant(const EnumVariantDecl& v) {
    out_ += v.name;
    if (v.hasPayload()) {
      out_ += '(';
      for (size_t j = 0; j < v.payloadTypes.size(); ++j) {
        if (j) out_ += ", ";
        printType(v.payloadTypes[j]);
      }
      out_ += ')';
    }
  }

  void printEnum(const EnumDefinitionAST& n) {
    out_ += "enum ";
    out_ += n.getName();
    const auto& typeParams = n.getTypeParameters();
    printTypeParams(typeParams);
    const auto& variants = n.getVariants();
    if (!isMultiLine(n.getLocation())) {
      out_ += " { ";
      for (size_t i = 0; i < variants.size(); ++i) {
        if (i) out_ += ", ";
        printVariant(variants[i]);
      }
      out_ += " }";
      return;
    }
    out_ += " {\n";
    ++indent_;
    for (size_t i = 0; i < variants.size(); ++i) {
      flushCommentsBefore(variants[i].location.offset);
      writeIndent();
      printVariant(variants[i]);
      if (i + 1 < variants.size()) out_ += ',';
      lastLine_ = endLineOf(variants[i].location);
      emitTrailingComments(lastLine_);
      out_ += '\n';
    }
    int endOffset =
        n.getLocation().endOffset.value_or(static_cast<int>(src_.size()));
    flushCommentsBefore(endOffset);
    --indent_;
    writeIndent();
    out_ += '}';
  }

  void printModule(const ModuleAST& m) {
    // module a.b.c parses to nested ModuleASTs sharing the same span start;
    // genuinely nested modules have distinct starts
    const ModuleAST* cur = &m;
    std::string dotted = cur->getName();
    while (true) {
      const auto& body = cur->getBody().getBody();
      if (body.size() == 1 && body[0] &&
          body[0]->getType() == ASTNodeType::MODULE &&
          body[0]->getLocation().offset == cur->getLocation().offset) {
        cur = static_cast<const ModuleAST*>(body[0].get());
        dotted += '.';
        dotted += cur->getName();
      } else {
        break;
      }
    }
    out_ += "module ";
    out_ += dotted;
    out_ += ' ';
    printBlockML(cur->getBody());
  }

  void printMatch(const MatchExprAST& m) {
    out_ += "match ";
    printExpr(*m.getDiscriminant());
    out_ += " {\n";
    ++indent_;
    const auto& arms = m.getArms();
    for (size_t i = 0; i < arms.size(); ++i) {
      const auto& arm = arms[i];
      if (arm.pattern) flushCommentsBefore(arm.pattern->getLocation().offset);
      writeIndent();
      int armEndLine = -1;
      if (arm.isWildcard) {
        out_ += '_';
      } else {
        printExpr(*arm.pattern);
        if (arm.hasPayloadParens) {
          out_ += '(';
          for (size_t j = 0; j < arm.bindings.size(); ++j) {
            if (j) out_ += ", ";
            out_ += arm.bindings[j].isWildcard ? "_" : arm.bindings[j].name;
          }
          out_ += ')';
        }
      }
      out_ += " => ";
      if (arm.body) {
        printExpr(*arm.body);
        armEndLine = endLineOf(arm.body->getLocation());
      }
      if (i + 1 < arms.size()) out_ += ',';
      if (armEndLine >= 0) {
        lastLine_ = armEndLine;
        emitTrailingComments(armEndLine);
      }
      out_ += '\n';
    }
    int endOffset =
        m.getLocation().endOffset.value_or(static_cast<int>(src_.size()));
    flushCommentsBefore(endOffset);
    --indent_;
    writeIndent();
    out_ += '}';
  }

  void printTryCatch(const TryCatchExprAST& t) {
    out_ += "try ";
    printBlockML(t.getTryBlock());
    for (const auto& clause : t.getCatchClauses()) {
      out_ += " catch (";
      out_ += clause.bindingName;
      if (clause.bindingType.has_value()) {
        out_ += ": ";
        printType(*clause.bindingType);
      }
      out_ += ") ";
      printBlockML(*clause.body);
    }
  }

  void printIf(const IfExprAST& n) {
    out_ += "if ";
    printExpr(*n.getCond());
    out_ += ' ';
    // Bodies are always blocks in the lossless tree
    printBlockML(static_cast<const BlockExprAST&>(*n.getThen()));
    const ExprAST* elseNode = n.getElse();
    if (!elseNode) return;
    if (elseNode->getType() == ASTNodeType::IF) {
      out_ += " else ";
      printIf(static_cast<const IfExprAST&>(*elseNode));
    } else {
      out_ += " else ";
      printBlockML(static_cast<const BlockExprAST&>(*elseNode));
    }
  }

  void printLambda(const LambdaAST& l) {
    out_ += "lambda ";
    const auto& caps = l.getProto().getRefCaptureNames();
    const auto& owned = l.getProto().getOwnedCaptureNames();
    if (!caps.empty() || !owned.empty()) {
      out_ += "[";
      for (size_t i = 0; i < caps.size(); ++i) {
        if (i) out_ += ", ";
        if (l.getProto().isConstRefCapture(caps[i])) out_ += "const ";
        out_ += "ref ";
        out_ += caps[i];
      }
      for (size_t i = 0; i < owned.size(); ++i) {
        if (i || !caps.empty()) out_ += ", ";
        out_ += owned[i];
      }
      out_ += "] ";
    }
    printProtoSig(l.getProto());  // lambda protos have an empty name
    out_ += ' ';
    printBlockAuto(l.getBody());
  }

  // --- the big dispatch -----------------------------------------------------

  void printExpr(const ExprAST& e) {
    const Position& loc = e.getLocation();
    switch (e.getType()) {
      // Literals: verbatim source slice (escapes, float spellings preserved)
      case ASTNodeType::NUMBER:
      case ASTNodeType::STRING_LITERAL:
      case ASTNodeType::CHAR_LITERAL:
      case ASTNodeType::BOOL_LITERAL:
      case ASTNodeType::NULL_LITERAL:
      case ASTNodeType::INTERPOLATED_STRING: {
        std::string s = slice(loc);
        out_ += s.empty() ? e.toString() : s;
        break;
      }

      case ASTNodeType::STRUCT_LITERAL: {
        const auto& lit = static_cast<const StructLiteralAST&>(e);
        out_ += '{';
        bool first = true;
        for (const auto& field : lit.getFields()) {
          out_ += first ? " " : ", ";
          first = false;
          out_ += field.name;
          out_ += ": ";
          printExpr(*field.value);
        }
        out_ += first ? "}" : " }";
        break;
      }
      case ASTNodeType::ARRAY_LITERAL: {
        const auto& n = static_cast<const ArrayLiteralAST&>(e);
        printExprList(n.getElements(), '[', ']', loc);
        break;
      }

      case ASTNodeType::PAREN_EXPR: {
        const auto& n = static_cast<const ParenExprAST&>(e);
        out_ += '(';
        printExpr(*n.getInner());
        out_ += ')';
        break;
      }

      case ASTNodeType::VARIABLE_REFERENCE:
        out_ += static_cast<const VariableReferenceAST&>(e).getName();
        break;

      case ASTNodeType::VARIABLE_CREATION: {
        const auto& n = static_cast<const VariableCreationAST&>(e);
        out_ += n.isConst() ? "const " : "var ";
        out_ += n.getName();
        if (n.hasTypeAnnotation()) {
          out_ += ": ";
          printType(*n.getTypeAnnotation());
        }
        if (n.getValue()) {
          out_ += " = ";
          printExpr(*n.getValue());
        }
        break;
      }

      case ASTNodeType::VARIABLE_ASSIGNMENT: {
        const auto& n = static_cast<const VariableAssignmentAST&>(e);
        out_ += n.getName();
        out_ += " = ";
        printExpr(*n.getValue());
        break;
      }

      case ASTNodeType::REFERENCE_CREATION: {
        const auto& n = static_cast<const ReferenceCreationAST&>(e);
        out_ += n.isMutable() ? "ref " : "const ref ";
        out_ += n.getName();
        out_ += " = ";
        printExpr(*n.getTarget());
        break;
      }

      case ASTNodeType::BINARY: {
        const auto& n = static_cast<const BinaryExprAST&>(e);
        printExpr(*n.getLHS());
        out_ += ' ';
        out_ += n.getOp().text;
        out_ += ' ';
        printExpr(*n.getRHS());
        break;
      }

      case ASTNodeType::UNARY: {
        const auto& n = static_cast<const UnaryExprAST&>(e);
        out_ += n.getOp().text;
        // Word operators (not) need a separating space
        if (std::isalpha(static_cast<unsigned char>(n.getOp().text[0]))) {
          out_ += ' ';
        }
        printExpr(*n.getOperand());
        break;
      }

      case ASTNodeType::TERNARY: {
        const auto& n = static_cast<const TernaryExprAST&>(e);
        printExpr(*n.getCond());
        out_ += " ? ";
        printExpr(*n.getThen());
        out_ += " : ";
        printExpr(*n.getElse());
        break;
      }

      case ASTNodeType::COMPOUND_ASSIGNMENT: {
        const auto& n = static_cast<const CompoundAssignmentAST&>(e);
        printExpr(*n.getTarget());
        out_ += ' ';
        out_ += n.getOp().text;
        out_ += ' ';
        printExpr(*n.getValue());
        break;
      }

      case ASTNodeType::INDEXED_ASSIGNMENT: {
        const auto& n = static_cast<const IndexedAssignmentAST&>(e);
        printExpr(*n.getTarget());
        out_ += " = ";
        printExpr(*n.getValue());
        break;
      }

      case ASTNodeType::MEMBER_ASSIGNMENT: {
        const auto& n = static_cast<const MemberAssignmentAST&>(e);
        printExpr(*n.getObject());
        out_ += '.';
        out_ += n.getMemberName();
        out_ += " = ";
        printExpr(*n.getValue());
        break;
      }

      case ASTNodeType::INDEX: {
        const auto& n = static_cast<const IndexAST&>(e);
        printExpr(*n.getTarget());
        out_ += '[';
        const auto& indices = n.getIndices();
        for (size_t i = 0; i < indices.size(); ++i) {
          if (i) out_ += ", ";
          printSlice(*indices[i]);
        }
        out_ += ']';
        break;
      }

      case ASTNodeType::CALL: {
        const auto& n = static_cast<const CallExprAST&>(e);
        printExpr(*n.getCallee());
        printExprList(n.getArgs(), '(', ')', loc);
        break;
      }

      case ASTNodeType::GENERIC_CALL: {
        const auto& n = static_cast<const GenericCallAST&>(e);
        out_ += n.getFunctionName();
        out_ += '<';
        const auto& typeArgs = n.getTypeArguments();
        for (size_t i = 0; i < typeArgs.size(); ++i) {
          if (i) out_ += ", ";
          printType(*typeArgs[i]);
        }
        out_ += '>';
        printExprList(n.getArgs(), '(', ')', loc);
        break;
      }

      case ASTNodeType::MEMBER_ACCESS: {
        const auto& n = static_cast<const MemberAccessAST&>(e);
        printExpr(*n.getObject());
        out_ += '.';
        out_ += n.getMemberName();
        if (n.hasTypeArguments()) {
          out_ += '<';
          const auto& typeArgs = n.getTypeArguments();
          for (size_t i = 0; i < typeArgs.size(); ++i) {
            if (i) out_ += ", ";
            printType(*typeArgs[i]);
          }
          out_ += '>';
        }
        break;
      }

      case ASTNodeType::THIS:
        out_ += "this";
        break;

      case ASTNodeType::QUALIFIED_NAME:
        out_ += static_cast<const QualifiedNameAST&>(e).getFullName();
        break;

      case ASTNodeType::PACK_EXPANSION:
        out_ += static_cast<const PackExpansionAST&>(e).getPackName();
        out_ += "...";
        break;

      case ASTNodeType::LAMBDA:
        printLambda(static_cast<const LambdaAST&>(e));
        break;

      case ASTNodeType::FUNCTION:
        printFunction(static_cast<const FunctionAST&>(e));
        break;

      case ASTNodeType::IF:
        printIf(static_cast<const IfExprAST&>(e));
        break;

      case ASTNodeType::WHILE_LOOP: {
        // While parens are structural (eaten by the parser), unlike if
        // conditions where they arrive as a ParenExprAST
        const auto& n = static_cast<const WhileExprAST&>(e);
        out_ += "while (";
        printExpr(*n.getCondition());
        out_ += ") ";
        printBlockML(static_cast<const BlockExprAST&>(*n.getBody()));
        break;
      }

      case ASTNodeType::FOR_LOOP: {
        const auto& n = static_cast<const ForExprAST&>(e);
        out_ += "for (";
        if (n.getInit()) printExpr(*n.getInit());
        out_ += "; ";
        if (n.getCondition()) printExpr(*n.getCondition());
        out_ += "; ";
        if (n.getIncrement()) printExpr(*n.getIncrement());
        out_ += ") ";
        printBlockML(static_cast<const BlockExprAST&>(*n.getBody()));
        break;
      }

      case ASTNodeType::FOR_IN_LOOP: {
        const auto& n = static_cast<const ForInExprAST&>(e);
        out_ += n.isConst() ? "for (const " : "for (var ";
        out_ += n.getLoopVar();
        out_ += ": ";
        printType(n.getLoopVarType());
        out_ += " in ";
        printExpr(*n.getIterable());
        out_ += ") ";
        printBlockML(static_cast<const BlockExprAST&>(*n.getBody()));
        break;
      }

      case ASTNodeType::BLOCK:
        printBlockML(static_cast<const BlockExprAST&>(e));
        break;

      case ASTNodeType::UNSAFE_BLOCK: {
        const auto& n = static_cast<const UnsafeBlockAST&>(e);
        out_ += "unsafe ";
        printBlockAuto(n.getBody());
        break;
      }

      case ASTNodeType::RETURN: {
        const auto& n = static_cast<const ReturnExprAST&>(e);
        out_ += "return";
        if (n.hasValue()) {
          out_ += ' ';
          printExpr(*n.getValue());
        }
        break;
      }

      case ASTNodeType::THROW: {
        const auto& n = static_cast<const ThrowExprAST&>(e);
        out_ += "throw";
        if (n.hasErrorExpr()) {
          out_ += ' ';
          printExpr(n.getErrorExpr());
        }
        break;
      }

      case ASTNodeType::BREAK_STMT:
        out_ += "break";
        break;

      case ASTNodeType::CONTINUE_STMT:
        out_ += "continue";
        break;

      case ASTNodeType::MATCH:
        printMatch(static_cast<const MatchExprAST&>(e));
        break;

      case ASTNodeType::TRY_CATCH:
        printTryCatch(static_cast<const TryCatchExprAST&>(e));
        break;

      case ASTNodeType::CLASS_DEFINITION:
        printClass(static_cast<const ClassDefinitionAST&>(e));
        break;

      case ASTNodeType::INTERFACE_DEFINITION:
        printInterface(static_cast<const InterfaceDefinitionAST&>(e));
        break;

      case ASTNodeType::ENUM_DEFINITION:
        printEnum(static_cast<const EnumDefinitionAST&>(e));
        break;

      case ASTNodeType::MODULE:
        printModule(static_cast<const ModuleAST&>(e));
        break;

      // Statement forms without structured spans on their parts (using,
      // declare, manifest, import): verbatim slice, normalized ';'
      default: {
        std::string s = stripPublic(slice(loc));
        if (s.empty()) {
          out_ += stripPublic(e.toString());
          break;
        }
        while (!s.empty() &&
               (std::isspace(static_cast<unsigned char>(s.back())) ||
                s.back() == ';')) {
          s.pop_back();
        }
        out_ += s;
        break;
      }
    }
  }

  void printSlice(const SliceExprAST& s) {
    if (!s.isRange()) {
      if (s.hasStart()) printExpr(*s.getStart());
      return;
    }
    if (s.hasStart()) printExpr(*s.getStart());
    out_ += ':';
    if (s.hasEnd()) printExpr(*s.getEnd());
  }
};

}  // namespace

std::string formatProgram(const BlockExprAST& program,
                          const std::map<int, Comment>& comments,
                          const std::string& source) {
  Formatter fmt(source, comments);
  return fmt.format(program);
}

std::string formatSource(const std::string& source,
                         const std::string& filePath) {
  std::istringstream dummy("");
  Parser parser(dummy);
  parser.setCollectComments(true);
  parser.setFilePath(filePath);
  auto program = parser.parseString(source);
  if (!program) {
    throw SunError(SunError::Kind::Parse,
                   "formatting failed: could not parse " + filePath);
  }
  return formatProgram(*program, parser.getComments(), source);
}

}  // namespace sun
