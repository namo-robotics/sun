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

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::EnumDefinitionAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::IfExprAST;
using sun::ast::ModuleAST;
using sun::support::Position;

/** Turns source text into syntax trees and provides source formatting. */
namespace sun::parsing {
/** Keeps the implementation helpers in this file private to this translation unit. */
namespace {

constexpr int kIndentWidth = 2;

/** Connects source text and recorded comments to the syntax formatter. */
class Formatter {
 public:
  /** Connects source text and recorded comments to the syntax formatter. */
  Formatter(const std::string& src,
            const std::map<int, sun::parsing::Comment>& comments)
      : src_(src), comments_(comments), next_(comments.begin()) {}

  /** Formats a parsed program while preserving its recorded comments. */
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
  const std::map<int, sun::parsing::Comment>& comments_;
  std::map<int, sun::parsing::Comment>::const_iterator next_;
  std::string out_;
  int indent_ = 0;
  int lastLine_ = -1;  // Source end line of the last emitted element; -1
                       // suppresses the blank-line check (start of scope)

  // --- small helpers -----------------------------------------------------

  /** Returns the last source line covered by a position span. */
  static int endLineOf(const Position& p) { return p.endLine.value_or(p.line); }

  /** Reports whether a source span crosses a line boundary. */
  static bool isMultiLine(const Position& p) {
    return p.endLine.has_value() && *p.endLine > p.line;
  }

  /** Returns the original source text within a position span. */
  std::string slice(const Position& p) const {
    if (!p.endOffset.has_value()) return "";
    return src_.substr(p.offset, *p.endOffset - p.offset);
  }

  /** Writes indentation for the current formatting depth. */
  void writeIndent() { out_.append(indent_ * kIndentWidth, ' '); }

  /**
   * Preserve at most one blank line from the source
   */
  void blankGap(int nextStartLine) {
    if (lastLine_ >= 0 && nextStartLine - lastLine_ >= 2) out_ += '\n';
  }

  /** Reports whether this object has comment before. */
  bool hasCommentBefore(int offset) const {
    return next_ != comments_.end() && next_->first < offset;
  }

  /**
   * Own-line emission of every comment starting before `offset`
   */
  void flushCommentsBefore(int offset) {
    while (next_ != comments_.end() && next_->first < offset) {
      const sun::parsing::Comment& c = next_->second;
      blankGap(c.span.line);
      writeIndent();
      out_ += c.text;  // multi-line block comments keep their raw interior
      out_ += '\n';
      lastLine_ = endLineOf(c.span);
      ++next_;
    }
  }

  /**
   * Comments on the same source line as the element just printed are
   * appended as trailing comments (two spaces before, gofmt-style)
   */
  void emitTrailingComments(int elemEndLine) {
    while (next_ != comments_.end() && !next_->second.ownLine &&
           next_->second.span.line == elemEndLine) {
      const sun::parsing::Comment& c = next_->second;
      out_ += "  ";
      out_ += c.text;
      lastLine_ = endLineOf(c.span);
      ++next_;
    }
  }

  // --- statements ---------------------------------------------------------

  /** Formats statements in order, beginning at the requested index. */
  void printStmts(const std::vector<std::unique_ptr<ExprAST>>& stmts,
                  size_t start = 0) {
    for (size_t i = start; i < stmts.size(); ++i) {
      const auto& stmt = stmts[i];
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

  /** Formats a statement and its required terminator. */
  void printStmt(const ExprAST& e) {
    printVisibility(e.getVisibility());
    printExpr(e);
    if (needsSemicolon(e)) out_ += ';';
  }

  /** Writes the access modifier for a declaration. */
  void printVisibility(sun::semantic_analysis::Visibility v) {
    if (v == sun::semantic_analysis::Visibility::Public) out_ += "public ";
  }

  /**
   * Drop a leading `public` from a verbatim slice; the modifier is re-emitted
   * by printVisibility so it is not doubled.
   */
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

  /** Reports whether a formatted expression requires a statement terminator. */
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

  /**
   * Multiline block: '{' newline, indented statements, '}'
   */
  void printBlockML(const BlockExprAST& b, size_t start = 0) {
    const Position& loc = b.getLocation();
    int endOffset = loc.endOffset.value_or(static_cast<int>(src_.size()));
    if (b.getBody().size() == start && !hasCommentBefore(endOffset)) {
      out_ += "{}";
      return;
    }
    out_ += "{\n";
    ++indent_;
    int savedLast = lastLine_;
    lastLine_ = -1;  // no blank line right after '{'
    printStmts(b.getBody(), start);
    flushCommentsBefore(endOffset);
    --indent_;
    writeIndent();
    out_ += '}';
    lastLine_ = savedLast;
  }

  /**
   * Block that may stay on one line when the source kept it on one line
   * (lambda and unsafe bodies); comments force the multiline form
   */
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

  /** Writes a type annotation using Sun source syntax. */
  void printType(const sun::ast::TypeAnnotation& t) {
    if (t.isFunction()) {
      if (t.requiresUnsafe) out_ += "unsafe ";
      out_ += "function (";
      for (size_t i = 0; i < t.paramTypes.size(); ++i) {
        if (i) out_ += ", ";
        printType(*t.paramTypes[i]);
      }
      out_ += ") ";
      if (t.returnType) {
        printType(*t.returnType);
      } else {
        out_ += "void";
      }
      if (t.canError) out_ += " throws IError";
      return;
    }
    if (t.span.endOffset.has_value()) {
      out_ += slice(t.span);
    } else {
      out_ += t.toString();  // fallback; parsed types always carry spans
    }
  }

  /**
   * `<'a, T, U: _Numeric>`, or nothing at all when the declaration has no
   * parameters. Lifetimes come first, as they are written.
   */
  void printTypeParams(
      const std::vector<sun::ast::TypeParameter>& typeParams,
      const std::vector<sun::ast::LifetimeParameter>& lifetimes = {}) {
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
      out_ += typeParam.name;
      if (typeParam.constraint) {
        const auto& constraint = *typeParam.constraint;
        out_ += ": " + constraint.name;
        if (!constraint.typeArguments.empty()) {
          out_ += '<';
          for (size_t i = 0; i < constraint.typeArguments.size(); ++i) {
            if (i) out_ += ", ";
            printType(constraint.typeArguments[i]);
          }
          out_ += '>';
        }
      }
      first = false;
    }
    out_ += '>';
  }

  /** Writes the selected parts of a function signature. */
  void printProtoSig(const sun::ast::PrototypeAST& p,
                     bool includeParameters = true,
                     bool includeReturnType = true) {
    out_ += p.getName();
    if (includeParameters) {
      printTypeParams(p.getTypeParameters(), p.getLifetimeParameters());
    }
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
    if (includeReturnType && p.getReturnType().has_value() &&
        p.getReturnType()->span.endOffset.has_value()) {
      out_ += ' ';
      printType(*p.getReturnType());
    } else if (includeReturnType && p.getReturnType().has_value() &&
               p.getReturnType()->canError) {
      // A throwing constructor spells only the error part:
      // init(...) throws IError
      out_ += " throws IError";
    }
  }

  /**
   * `asMethod` prints the class/interface member keyword instead of
   * 'function'; the rest of the signature is spelled the same way.
   */
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
    out_ += asMethod ? "method " : f.isTest() ? "test_function " : "function ";
    // A test's whole return signature (void, throws IError) is synthesized,
    // never spelled in source, so printing any of it would not re-parse.
    printProtoSig(f.getProto(), /*includeParameters=*/true,
                  /*includeReturnType=*/!f.isTest());
    out_ += ' ';
    printBlockML(f.getBody());
  }

  // --- expression lists -----------------------------------------------------

  /**
   * Single line: (a, b, c). Multiline (when the construct spanned lines in
   * the source): one element per line, closing bracket at parent indent.
   */
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

  /** Writes a class declaration, including its fields and methods. */
  void printClass(const sun::ast::ClassDefinitionAST& c) {
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

    /**
     * Fields and methods live in separate vectors; merge by source order
     */
    struct Member {
      int offset;
      int line;
      int endLine;
      const sun::ast::ClassFieldDecl* field;
      const FunctionAST* method;
    };
    std::vector<Member> members;
    for (const auto& f : c.getFields()) {
      members.push_back(
          {f.location.offset, f.location.line,
           endLineOf(f.initializer ? f.initializer->getLocation() : f.location),
           &f, nullptr});
    }
    for (const auto& m : c.getMethods()) {
      if (m.function->isSynthesizedConstructor()) continue;
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
        if (m.field->initializer) {
          out_ += " = ";
          printExpr(*m.field->initializer);
        }
        out_ += ';';
      } else {
        const std::string& methodName = m.method->getProto().getName();
        if (methodName == "init" || methodName == "deinit") {
          // Constructors and destructors are written bare: init(...) { }
          printProtoSig(m.method->getProto());
          out_ += ' ';
          printBlockML(m.method->getBody(),
                       m.method->getFieldInitializerCount());
        } else {
          printVisibility(m.method->getVisibility());
          if (m.method->getProto().isConstMethod()) out_ += "const ";
          if (m.method->getProto().isUnsafeMethod()) out_ += "unsafe ";
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

  /** Writes an interface declaration and its member requirements. */
  void printInterface(const sun::ast::InterfaceDefinitionAST& n) {
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
      const sun::ast::InterfaceFieldDecl* field;
      const sun::ast::InterfaceMethodDecl* method;
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
        if (m.method->function->getProto().isUnsafeMethod()) out_ += "unsafe ";
        // Signature-only method (the parser synthesizes an empty body)
        out_ += "method ";
        printProtoSig(m.method->function->getProto());
        out_ += ';';
      } else {
        printVisibility(m.method->visibility());
        if (m.method->isConst) out_ += "const ";
        if (m.method->function->getProto().isUnsafeMethod()) out_ += "unsafe ";
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

  /** Writes an enum alternative and any payload fields. */
  void printVariant(const sun::ast::EnumVariantDecl& v,
                    const EnumDefinitionAST& n) {
    out_ += v.name;
    if (v.hasPayload()) {
      out_ += '(';
      for (size_t j = 0; j < v.payloadTypes.size(); ++j) {
        if (j) out_ += ", ";
        printType(v.payloadTypes[j]);
      }
      out_ += ')';
    }
    if (v.hasExplicitValue) out_ += " = " + n.getValueText(v);
  }

  /** Writes an enum declaration and its alternatives. */
  void printEnum(const EnumDefinitionAST& n) {
    out_ += "enum ";
    out_ += n.getName();
    const auto& typeParams = n.getTypeParameters();
    printTypeParams(typeParams);
    if (!n.getUnderlyingType().empty()) out_ += " " + n.getUnderlyingType();
    const auto& variants = n.getVariants();
    if (!isMultiLine(n.getLocation())) {
      out_ += " { ";
      for (size_t i = 0; i < variants.size(); ++i) {
        if (i) out_ += ", ";
        printVariant(variants[i], n);
      }
      out_ += " }";
      return;
    }
    out_ += " {\n";
    ++indent_;
    lastLine_ = -1;  // no blank line right after '{'
    for (size_t i = 0; i < variants.size(); ++i) {
      flushCommentsBefore(variants[i].location.offset);
      writeIndent();
      printVariant(variants[i], n);
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

  /** Writes a module declaration using formatted Sun source syntax. */
  void printModule(const ModuleAST& m) {
    const ModuleAST* cur = &m;
    std::string dotted = cur->getName();
    while (const auto* inner = cur->getShorthandChild()) {
      dotted += '.';
      dotted += inner->getName();
      cur = inner;
    }
    out_ += "module ";
    out_ += dotted;
    out_ += ' ';
    printBlockML(cur->getBody());
  }

  /** Writes a pattern match using formatted Sun source syntax. */
  void printMatch(const sun::ast::MatchExprAST& m) {
    out_ += "match ";
    printExpr(*m.getDiscriminant());
    out_ += " {\n";
    ++indent_;
    lastLine_ = -1;  // no blank line right after '{'
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

  /** Writes a error handler using formatted Sun source syntax. */
  void printTryCatch(const sun::ast::TryCatchExprAST& t) {
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

  /** Writes a conditional expression using formatted Sun source syntax. */
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

  /** Writes a lambda expression using formatted Sun source syntax. */
  void printLambda(const sun::ast::LambdaAST& l) {
    printTypeParams(l.getProto().getTypeParameters(),
                    l.getProto().getLifetimeParameters());
    const auto& caps = l.getProto().getRefCaptureNames();
    const auto& owned = l.getProto().getOwnedCaptureNames();
    if (!caps.empty() || !owned.empty()) {
      if (!l.getProto().getLifetimeParameters().empty()) out_ += ' ';
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
      out_ += "]";
    }
    // Lambda prototypes have no name. Their return type follows the fat arrow.
    printProtoSig(l.getProto(), false, false);
    out_ += " => ";
    printType(*l.getProto().getReturnType());
    out_ += ' ';
    printBlockAuto(l.getBody());
  }

  // --- the big dispatch -----------------------------------------------------

  /** Writes an expression using formatted Sun source syntax. */
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
        const auto& lit = static_cast<const sun::ast::StructLiteralAST&>(e);
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
        const auto& n = static_cast<const sun::ast::ArrayLiteralAST&>(e);
        printExprList(n.getElements(), '[', ']', loc);
        break;
      }

      case ASTNodeType::PAREN_EXPR: {
        const auto& n = static_cast<const sun::ast::ParenExprAST&>(e);
        out_ += '(';
        printExpr(*n.getInner());
        out_ += ')';
        break;
      }

      case ASTNodeType::VARIABLE_REFERENCE:
        out_ += static_cast<const sun::ast::VariableReferenceAST&>(e).getName();
        break;

      case ASTNodeType::VARIABLE_CREATION: {
        const auto& n = static_cast<const sun::ast::VariableCreationAST&>(e);
        if (n.isCExtern()) {
          // Preserve the optional ABI spelling from the source span.
          std::string s = stripPublic(slice(n.getLocation()));
          bool explicitAbi =
              n.hasExplicitCAbi() || s.rfind("extern \"C\"", 0) == 0;
          out_ += explicitAbi ? "extern \"C\" var " : "extern var ";
        } else {
          out_ += n.isConst() ? "const " : "var ";
        }
        out_ += n.getName();
        if (n.hasTypeAnnotation()) {
          out_ += ": ";
          printType(*n.getTypeAnnotation());
        }
        if (n.getValue()) {
          out_ += " = ";
          printExpr(*n.getValue());
        }
        if (n.hasLinkName()) {
          out_ += " as \"" + n.getLinkName() + '"';
        }
        break;
      }

      case ASTNodeType::VARIABLE_ASSIGNMENT: {
        const auto& n = static_cast<const sun::ast::VariableAssignmentAST&>(e);
        out_ += n.getName();
        out_ += " = ";
        printExpr(*n.getValue());
        break;
      }

      case ASTNodeType::REFERENCE_CREATION: {
        const auto& n = static_cast<const sun::ast::ReferenceCreationAST&>(e);
        out_ += n.isMutable() ? "ref " : "const ref ";
        out_ += n.getName();
        out_ += " = ";
        printExpr(*n.getTarget());
        break;
      }

      case ASTNodeType::BINARY: {
        const auto& n = static_cast<const sun::ast::BinaryExprAST&>(e);
        printExpr(*n.getLHS());
        out_ += ' ';
        out_ += n.getOp().text;
        out_ += ' ';
        printExpr(*n.getRHS());
        break;
      }

      case ASTNodeType::UNARY: {
        const auto& n = static_cast<const sun::ast::UnaryExprAST&>(e);
        out_ += n.getOp().text;
        // Word operators (not) need a separating space
        if (std::isalpha(static_cast<unsigned char>(n.getOp().text[0]))) {
          out_ += ' ';
        }
        printExpr(*n.getOperand());
        break;
      }

      case ASTNodeType::TERNARY: {
        const auto& n = static_cast<const sun::ast::TernaryExprAST&>(e);
        printExpr(*n.getCond());
        out_ += " ? ";
        printExpr(*n.getThen());
        out_ += " : ";
        printExpr(*n.getElse());
        break;
      }

      case ASTNodeType::COMPOUND_ASSIGNMENT: {
        const auto& n = static_cast<const sun::ast::CompoundAssignmentAST&>(e);
        printExpr(*n.getTarget());
        out_ += ' ';
        out_ += n.getOp().text;
        out_ += ' ';
        printExpr(*n.getValue());
        break;
      }

      case ASTNodeType::INDEXED_ASSIGNMENT: {
        const auto& n = static_cast<const sun::ast::IndexedAssignmentAST&>(e);
        printExpr(*n.getTarget());
        out_ += " = ";
        printExpr(*n.getValue());
        break;
      }

      case ASTNodeType::MEMBER_ASSIGNMENT: {
        const auto& n = static_cast<const sun::ast::MemberAssignmentAST&>(e);
        printExpr(*n.getObject());
        out_ += '.';
        out_ += n.getMemberName();
        out_ += " = ";
        printExpr(*n.getValue());
        break;
      }

      case ASTNodeType::INDEX: {
        const auto& n = static_cast<const sun::ast::IndexAST&>(e);
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
        const auto& n = static_cast<const sun::ast::CallExprAST&>(e);
        printExpr(*n.getCallee());
        printExprList(n.getArgs(), '(', ')', loc);
        break;
      }

      case ASTNodeType::GENERIC_CALL: {
        const auto& n = static_cast<const sun::ast::GenericCallAST&>(e);
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
        const auto& n = static_cast<const sun::ast::MemberAccessAST&>(e);
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
        out_ += static_cast<const sun::ast::QualifiedNameAST&>(e).getFullName();
        break;

      case ASTNodeType::PACK_EXPANSION:
        out_ += static_cast<const sun::ast::PackExpansionAST&>(e).getPackName();
        out_ += "...";
        break;

      case ASTNodeType::LAMBDA:
        printLambda(static_cast<const sun::ast::LambdaAST&>(e));
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
        const auto& n = static_cast<const sun::ast::WhileExprAST&>(e);
        out_ += "while (";
        printExpr(*n.getCondition());
        out_ += ") ";
        printBlockML(static_cast<const BlockExprAST&>(*n.getBody()));
        break;
      }

      case ASTNodeType::FOR_LOOP: {
        const auto& n = static_cast<const sun::ast::ForExprAST&>(e);
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
        const auto& n = static_cast<const sun::ast::ForInExprAST&>(e);
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
        const auto& n = static_cast<const sun::ast::UnsafeBlockAST&>(e);
        out_ += "unsafe ";
        if (n.isExpressionForm())
          printExpr(*n.getBody().getLastExpr());
        else
          printBlockAuto(n.getBody());
        break;
      }

      case ASTNodeType::RETURN: {
        const auto& n = static_cast<const sun::ast::ReturnExprAST&>(e);
        out_ += "return";
        if (n.hasValue()) {
          out_ += ' ';
          printExpr(*n.getValue());
        }
        break;
      }

      case ASTNodeType::THROW: {
        const auto& n = static_cast<const sun::ast::ThrowExprAST&>(e);
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
        printMatch(static_cast<const sun::ast::MatchExprAST&>(e));
        break;

      case ASTNodeType::TRY_CATCH:
        printTryCatch(static_cast<const sun::ast::TryCatchExprAST&>(e));
        break;

      case ASTNodeType::CLASS_DEFINITION:
        printClass(static_cast<const sun::ast::ClassDefinitionAST&>(e));
        break;

      case ASTNodeType::INTERFACE_DEFINITION:
        printInterface(static_cast<const sun::ast::InterfaceDefinitionAST&>(e));
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

  /** Writes a slice expression using formatted Sun source syntax. */
  void printSlice(const sun::ast::SliceExprAST& s) {
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

/** Formats a parsed program with its original comments and source context. */
std::string formatProgram(const BlockExprAST& program,
                          const std::map<int, sun::parsing::Comment>& comments,
                          const std::string& source) {
  Formatter fmt(source, comments);
  return fmt.format(program);
}

/** Parses and formats Sun source text for the supplied file. */
std::string formatSource(const std::string& source,
                         const std::string& filePath) {
  std::istringstream dummy("");
  sun::parsing::Parser parser(dummy);
  parser.setCollectComments(true);
  parser.setFilePath(filePath);
  auto program = parser.parseString(source);
  if (!program) {
    throw sun::support::SunError(
        sun::support::SunError::Kind::Parse,
        "formatting failed: could not parse " + filePath);
  }
  return formatProgram(*program, parser.getComments(), source);
}

}  // namespace sun::parsing
