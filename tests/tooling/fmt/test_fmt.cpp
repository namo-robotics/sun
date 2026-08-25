// tests/tooling/fmt/test_fmt.cpp — sun fmt formatter tests

#include <gtest/gtest.h>

#include <string>

#include "parsing/formatter.h"
#include "support/error.h"

static std::string fmt(const std::string& src) {
  return sun::formatSource(src);
}

// ------------------------------------------------------------------
// Core layout
// ------------------------------------------------------------------

TEST(Tooling_Fmt, SimpleFunction) {
  EXPECT_EQ(fmt("function main()i32{return 42;}"),
            "function main() i32 {\n"
            "  return 42;\n"
            "}\n");
}

TEST(Tooling_Fmt, IndentNesting) {
  EXPECT_EQ(fmt("function main() i32 {\n"
                "if (1 < 2) {\n"
                "while (true) {\n"
                "break;\n"
                "}\n"
                "}\n"
                "return 0;\n"
                "}"),
            "function main() i32 {\n"
            "  if (1 < 2) {\n"
            "    while (true) {\n"
            "      break;\n"
            "    }\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
}

TEST(Tooling_Fmt, ElseIfChain) {
  EXPECT_EQ(fmt("function f(x: i32) i32 {\n"
                "if (x == 1) { return 1; } else if (x == 2) { return 2; }\n"
                "else { return 3; }\n"
                "}"),
            "function f(x: i32) i32 {\n"
            "  if (x == 1) {\n"
            "    return 1;\n"
            "  } else if (x == 2) {\n"
            "    return 2;\n"
            "  } else {\n"
            "    return 3;\n"
            "  }\n"
            "}\n");
}

TEST(Tooling_Fmt, ElseWithNestedIfStaysBraced) {
  // else { if ... } must NOT collapse to else if
  EXPECT_EQ(fmt("function f(x: i32) i32 {\n"
                "if (x == 1) { return 1; } else { if (x == 2) { return 2; } }\n"
                "return 3;\n"
                "}"),
            "function f(x: i32) i32 {\n"
            "  if (x == 1) {\n"
            "    return 1;\n"
            "  } else {\n"
            "    if (x == 2) {\n"
            "      return 2;\n"
            "    }\n"
            "  }\n"
            "  return 3;\n"
            "}\n");
}

TEST(Tooling_Fmt, EmptyBodiesPrintEmptyBraces) {
  // No phantom `false;` / `0;` from the old parser unwrapping
  EXPECT_EQ(fmt("function f() void {\n"
                "if (true) {}\n"
                "while (false) {}\n"
                "}"),
            "function f() void {\n"
            "  if (true) {}\n"
            "  while (false) {}\n"
            "}\n");
}

TEST(Tooling_Fmt, VarWithAndWithoutAnnotation) {
  EXPECT_EQ(fmt("function f() void {\n"
                "var x=5;\n"
                "var y:i32=6;\n"
                "}"),
            "function f() void {\n"
            "  var x = 5;\n"
            "  var y: i32 = 6;\n"
            "}\n");
}

TEST(Tooling_Fmt, OperatorsAndParens) {
  // Parens are preserved exactly, never added or removed
  EXPECT_EQ(fmt("function f() i32 { return (1+2)*-3; }"),
            "function f() i32 {\n"
            "  return (1 + 2) * -3;\n"
            "}\n");
}

TEST(Tooling_Fmt, TernaryAndCompound) {
  EXPECT_EQ(fmt("function f(x: i32) i32 {\n"
                "x+=2;\n"
                "return x>0?x:0-x;\n"
                "}"),
            "function f(x: i32) i32 {\n"
            "  x += 2;\n"
            "  return x > 0 ? x : 0 - x;\n"
            "}\n");
}

TEST(Tooling_Fmt, ForLoop) {
  EXPECT_EQ(fmt("function f() i32 {\n"
                "var s: i32 = 0;\n"
                "for (var i: i32 = 0;i<10;i+=1) { s += i; }\n"
                "return s;\n"
                "}"),
            "function f() i32 {\n"
            "  var s: i32 = 0;\n"
            "  for (var i: i32 = 0; i < 10; i += 1) {\n"
            "    s += i;\n"
            "  }\n"
            "  return s;\n"
            "}\n");
}

TEST(Tooling_Fmt, LiteralFidelity) {
  // Float spellings and string escapes are sliced verbatim from source
  EXPECT_EQ(fmt("function f() void {\n"
                "var a: f64 = 2.50;\n"
                "var b = \"tab\\there\";\n"
                "}"),
            "function f() void {\n"
            "  var a: f64 = 2.50;\n"
            "  var b = \"tab\\there\";\n"
            "}\n");
}

TEST(Tooling_Fmt, TypeFidelity) {
  EXPECT_EQ(fmt("function f(a: ref  Foo, b: raw_ptr<Bar>) i32,  IError {\n"
                "throw 1;\n"
                "}"),
            "function f(a: ref  Foo, b: raw_ptr<Bar>) i32,  IError {\n"
            "  throw 1;\n"
            "}\n");
}

// ------------------------------------------------------------------
// Blank lines
// ------------------------------------------------------------------

TEST(Tooling_Fmt, BlankLinesCollapseToOne) {
  EXPECT_EQ(fmt("function f() void {\n"
                "var a = 1;\n"
                "\n\n\n"
                "var b = 2;\n"
                "}"),
            "function f() void {\n"
            "  var a = 1;\n"
            "\n"
            "  var b = 2;\n"
            "}\n");
}

TEST(Tooling_Fmt, NoBlankAfterOpenBrace) {
  EXPECT_EQ(fmt("function f() void {\n\n\n  var a = 1;\n}"),
            "function f() void {\n"
            "  var a = 1;\n"
            "}\n");
}

TEST(Tooling_Fmt, BlankLineBetweenFunctions) {
  EXPECT_EQ(fmt("function a() void {}\n\nfunction b() void {}\n"),
            "function a() void {}\n"
            "\n"
            "function b() void {}\n");
}

// ------------------------------------------------------------------
// Comments
// ------------------------------------------------------------------

TEST(Tooling_Fmt, OwnLineComment) {
  EXPECT_EQ(fmt("// leading\nfunction f() void {\n// inner\nvar a = 1;\n}"),
            "// leading\n"
            "function f() void {\n"
            "  // inner\n"
            "  var a = 1;\n"
            "}\n");
}

TEST(Tooling_Fmt, TrailingComment) {
  EXPECT_EQ(fmt("function f() void {\nvar a = 1;  // note\n}"),
            "function f() void {\n"
            "  var a = 1;  // note\n"
            "}\n");
}

TEST(Tooling_Fmt, CommentBeforeCloseBrace) {
  EXPECT_EQ(fmt("function f() void {\nvar a = 1;\n// tail\n}"),
            "function f() void {\n"
            "  var a = 1;\n"
            "  // tail\n"
            "}\n");
}

TEST(Tooling_Fmt, CommentAtEndOfFile) {
  EXPECT_EQ(fmt("function f() void {}\n// bye\n"),
            "function f() void {}\n"
            "// bye\n");
}

TEST(Tooling_Fmt, BlockCommentOwnLine) {
  EXPECT_EQ(fmt("/* multi\n   line */\nfunction f() void {}\n"),
            "/* multi\n   line */\n"
            "function f() void {}\n");
}

TEST(Tooling_Fmt, InlineBlockCommentMovesToTrailing) {
  // v1 policy: comments inside an expression are re-attached as trailing
  EXPECT_EQ(fmt("function f() void {\nvar a = /* inline */ 1;\n}"),
            "function f() void {\n"
            "  var a = 1;  /* inline */\n"
            "}\n");
}

// ------------------------------------------------------------------
// Single-line / multi-line list preservation
// ------------------------------------------------------------------

TEST(Tooling_Fmt, SingleLineListStaysSingleLine) {
  EXPECT_EQ(fmt("function g(a: i32, b: i32) void {}\n"
                "function f() void { g(1,   2); }\n"),
            "function g(a: i32, b: i32) void {}\n"
            "function f() void {\n"
            "  g(1, 2);\n"
            "}\n");
}

TEST(Tooling_Fmt, MultiLineCallStaysMultiLine) {
  EXPECT_EQ(fmt("function g(a: i32, b: i32) void {}\n"
                "function f() void {\ng(1,\n2);\n}\n"),
            "function g(a: i32, b: i32) void {}\n"
            "function f() void {\n"
            "  g(\n"
            "    1,\n"
            "    2\n"
            "  );\n"
            "}\n");
}

TEST(Tooling_Fmt, ArrayLiterals) {
  EXPECT_EQ(fmt("function f() void {\nvar a = [1,2,3];\n}"),
            "function f() void {\n"
            "  var a = [1, 2, 3];\n"
            "}\n");
}

// ------------------------------------------------------------------
// Error handling
// ------------------------------------------------------------------

TEST(Tooling_Fmt, ParseErrorThrows) {
  EXPECT_THROW(fmt("function f( {"), SunError);
}

TEST(Tooling_Fmt, Idempotence) {
  std::string src =
      "// header\n"
      "function main() i32 {\n"
      "var x: i32 = 40;  // note\n"
      "\n"
      "if (x > 0) { x += 2; } else { x = 0; }\n"
      "return x;\n"
      "}";
  std::string once = fmt(src);
  EXPECT_EQ(fmt(once), once);
}

// ------------------------------------------------------------------
// Long tail: declarations and control flow
// ------------------------------------------------------------------

TEST(Tooling_Fmt, ClassWithFieldsAndMethods) {
  EXPECT_EQ(fmt("class Counter {\n"
                "var count: i32;\n"
                "function init(start: i32) { this.count = start; }\n"
                "\n"
                "function get() i32 { return this.count; }\n"
                "}"),
            "class Counter {\n"
            "  var count: i32;\n"
            "  function init(start: i32) {\n"
            "    this.count = start;\n"
            "  }\n"
            "\n"
            "  function get() i32 {\n"
            "    return this.count;\n"
            "  }\n"
            "}\n");
}

TEST(Tooling_Fmt, GenericClassImplements) {
  EXPECT_EQ(fmt("class Vec<T> implements IIterable<T,Vec<T>> {\n"
                "var len: i32;\n"
                "}"),
            "class Vec<T> implements IIterable<T, Vec<T>> {\n"
            "  var len: i32;\n"
            "}\n");
}

TEST(Tooling_Fmt, PartialClass) {
  EXPECT_EQ(fmt("partial class X {\nfunction m() void {}\n}"),
            "partial class X {\n"
            "  function m() void {}\n"
            "}\n");
}

TEST(Tooling_Fmt, InterfaceSignatureAndDefault) {
  EXPECT_EQ(fmt("interface IAllocator {\n"
                "function alloc(size: i64) raw_ptr<u8>;\n"
                "function zero() i32 { return 0; }\n"
                "}"),
            "interface IAllocator {\n"
            "  function alloc(size: i64) raw_ptr<u8>;\n"
            "  function zero() i32 {\n"
            "    return 0;\n"
            "  }\n"
            "}\n");
}

TEST(Tooling_Fmt, EnumSingleAndMultiLine) {
  EXPECT_EQ(fmt("enum Color { Red,Green,  Blue }"),
            "enum Color { Red, Green, Blue }\n");
  EXPECT_EQ(fmt("enum Color {\nRed,\nGreen,\nBlue\n}"),
            "enum Color {\n"
            "  Red,\n"
            "  Green,\n"
            "  Blue\n"
            "}\n");
}

TEST(Tooling_Fmt, MatchExpression) {
  EXPECT_EQ(fmt("function f(d: i32) i32 {\n"
                "return match d { 0 => 48, 1 => 49, _ => 57 };\n"
                "}"),
            "function f(d: i32) i32 {\n"
            "  return match d {\n"
            "    0 => 48,\n"
            "    1 => 49,\n"
            "    _ => 57\n"
            "  };\n"
            "}\n");
}

TEST(Tooling_Fmt, TryCatchThrow) {
  EXPECT_EQ(fmt("function f(x: i32) i32 {\n"
                "try { return div(10, x); } catch (e: IError) { return -1; }\n"
                "}"),
            "function f(x: i32) i32 {\n"
            "  try {\n"
            "    return div(10, x);\n"
            "  } catch (e: IError) {\n"
            "    return -1;\n"
            "  }\n"
            "}\n");
}

TEST(Tooling_Fmt, DottedModule) {
  EXPECT_EQ(fmt("module sun.io {\nfunction f() void {}\n}"),
            "module sun.io {\n"
            "  function f() void {}\n"
            "}\n");
}

TEST(Tooling_Fmt, NestedModulesStayNested) {
  EXPECT_EQ(fmt("module a {\nmodule b {\nfunction f() void {}\n}\n}"),
            "module a {\n"
            "  module b {\n"
            "    function f() void {}\n"
            "  }\n"
            "}\n");
}

TEST(Tooling_Fmt, UsingAndDeclare) {
  EXPECT_EQ(fmt("using sun;\n"
                "using sun.Vec;\n"
                "declare Vec_i32 = Vec<i32>;\n"
                "declare function isOdd(n: i32) bool;\n"
                "extern function _malloc(size: i64) raw_ptr<u8>;\n"),
            "using sun;\n"
            "using sun.Vec;\n"
            "declare Vec_i32 = Vec<i32>;\n"
            "declare function isOdd(n: i32) bool;\n"
            "extern function _malloc(size: i64) raw_ptr<u8>;\n");
}

TEST(Tooling_Fmt, LambdaAndCaptures) {
  EXPECT_EQ(fmt("function f() i32 {\n"
                "var add = lambda (x: i32) i32 { return x + 3; };\n"
                "var n: i32 = 1;\n"
                "var bump = lambda [ref n] () void { n += 1; };\n"
                "return add(1);\n"
                "}"),
            "function f() i32 {\n"
            "  var add = lambda (x: i32) i32 { return x + 3; };\n"
            "  var n: i32 = 1;\n"
            "  var bump = lambda [ref n] () void { n += 1; };\n"
            "  return add(1);\n"
            "}\n");
}

TEST(Tooling_Fmt, UnsafeBlockInlineAndMultiline) {
  EXPECT_EQ(fmt("function f(fd: i32) i32 {\n"
                "return unsafe { __file_read(fd, 1); };\n"
                "}"),
            "function f(fd: i32) i32 {\n"
            "  return unsafe { __file_read(fd, 1); };\n"
            "}\n");
}

TEST(Tooling_Fmt, ForInLoop) {
  EXPECT_EQ(fmt("function f(v: ref Vec<i32>) i32 {\n"
                "var s: i32 = 0;\n"
                "for (var x: i32 in v) { s += x; }\n"
                "return s;\n"
                "}"),
            "function f(v: ref Vec<i32>) i32 {\n"
            "  var s: i32 = 0;\n"
            "  for (var x: i32 in v) {\n"
            "    s += x;\n"
            "  }\n"
            "  return s;\n"
            "}\n");
}

TEST(Tooling_Fmt, IndexingAndSlices) {
  EXPECT_EQ(fmt("function f(a: array<i32, 10>) i32 {\n"
                "var b = a[1:5];\n"
                "var c = a[:];\n"
                "a[0] = 7;\n"
                "return a[2];\n"
                "}"),
            "function f(a: array<i32, 10>) i32 {\n"
            "  var b = a[1:5];\n"
            "  var c = a[:];\n"
            "  a[0] = 7;\n"
            "  return a[2];\n"
            "}\n");
}

TEST(Tooling_Fmt, RefCreationAndConst) {
  EXPECT_EQ(fmt("function f(v: ref Vec<i32>, c: const ref Vec<i32>) void {\n"
                "ref a = v;\n"
                "const ref b = v;\n"
                "const n: i64 = c.size();\n"
                "var r: const ref Vec<i32> = v;\n"
                "}"),
            "function f(v: ref Vec<i32>, c: const ref Vec<i32>) void {\n"
            "  ref a = v;\n"
            "  const ref b = v;\n"
            "  const n: i64 = c.size();\n"
            "  var r: const ref Vec<i32> = v;\n"
            "}\n");
}

TEST(Tooling_Fmt, ConstMethodsAndLoops) {
  EXPECT_EQ(fmt("class C {\n"
                "var n: i32;\n"
                "public const function get() i32 { return this.n; }\n"
                "const function zero() bool { return this.n == 0; }\n"
                "}\n"
                "interface I {\n"
                "const function get() i32;\n"
                "}\n"
                "function f(v: ref Vec<i32>) void {\n"
                "for (const x: i32 in v) { }\n"
                "}"),
            "class C {\n"
            "  var n: i32;\n"
            "  public const function get() i32 {\n"
            "    return this.n;\n"
            "  }\n"
            "  const function zero() bool {\n"
            "    return this.n == 0;\n"
            "  }\n"
            "}\n"
            "interface I {\n"
            "  const function get() i32;\n"
            "}\n"
            "function f(v: ref Vec<i32>) void {\n"
            "  for (const x: i32 in v) {}\n"
            "}\n");
}

TEST(Tooling_Fmt, InterpolatedString) {
  EXPECT_EQ(fmt("function f(x: i32) void {\n"
                "var s = `value: ${x + 1}!`;\n"
                "}"),
            "function f(x: i32) void {\n"
            "  var s = `value: ${x + 1}!`;\n"
            "}\n");
}

TEST(Tooling_Fmt, GenericCallAndMemberChain) {
  EXPECT_EQ(fmt("function f() void {\n"
                "var v = create<Vec<i32>>(1, 2);\n"
                "v.items().first().print();\n"
                "}"),
            "function f() void {\n"
            "  var v = create<Vec<i32>>(1, 2);\n"
            "  v.items().first().print();\n"
            "}\n");
}

TEST(Tooling_Fmt, MultipleFilesOneParserNoCommentBleed) {
  // Regression: parseString clears the comment table between inputs
  EXPECT_EQ(fmt("// first\nfunction a() void {}\n"),
            "// first\nfunction a() void {}\n");
  EXPECT_EQ(fmt("function b() void {}\n"), "function b() void {}\n");
}

// ------------------------------------------------------------------
// Property tests over the corpus (tests/programs + stdlib)
// ------------------------------------------------------------------

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "parsing/lowering_pass.h"
#include "parsing/parser.h"
#include "serialization/ast_serializer.h"

namespace {

std::vector<std::filesystem::path> corpusFiles() {
  std::vector<std::filesystem::path> files;
  for (const char* dir : {"tests/programs", "stdlib", "examples"}) {
    if (!std::filesystem::exists(dir)) continue;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.path().extension() == ".sun") files.push_back(entry.path());
    }
  }
  return files;
}

std::string readFile(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Post-lowering, position-free serialization for structural comparison.
// Mutates the tree (lowering runs in place); use only when done with it.
std::string loweredFingerprint(BlockExprAST& ast) {
  LoweringPass lowering;
  lowering.run(ast);
  sun::serialization::SerializerConfig config;
  config.include_analysis = false;
  config.include_location = false;
  sun::serialization::ASTSerializer serializer(config);
  return serializer.serializeProgram(ast).SerializeAsString();
}

}  // namespace

TEST(Tooling_Fmt_Corpus, IdempotentAndStructurePreserving) {
  // One shared parser; each corpus file is parsed exactly twice (original and
  // formatted)
  std::istringstream dummy("");
  Parser parser(dummy);
  parser.setCollectComments(true);

  int checked = 0;
  for (const auto& path : corpusFiles()) {
    std::string source = readFile(path);
    std::unique_ptr<BlockExprAST> ast;
    try {
      ast = parser.parseString(source);
    } catch (const SunError&) {
      continue;  // legacy files (import statements) don't parse standalone
    }
    if (!ast) continue;
    ++checked;

    std::string formatted =
        sun::formatProgram(*ast, parser.getComments(), source);
    std::string originalFp = loweredFingerprint(*ast);

    // Re-parses cleanly and is idempotent
    std::unique_ptr<BlockExprAST> ast2;
    ASSERT_NO_THROW(ast2 = parser.parseString(formatted)) << path;
    ASSERT_NE(ast2, nullptr) << path;
    std::string reformatted =
        sun::formatProgram(*ast2, parser.getComments(), formatted);
    EXPECT_EQ(reformatted, formatted) << path;
    // Same post-lowering structure as the original
    EXPECT_EQ(loweredFingerprint(*ast2), originalFp) << path;
  }
  // The corpus must actually be exercised
  EXPECT_GE(checked, 15) << "corpus unexpectedly small";
}

// ------------------------------------------------------------------
// Payload enums + match destructuring
// ------------------------------------------------------------------

TEST(Tooling_Fmt, EnumPayloadVariants) {
  EXPECT_EQ(fmt("enum Shape{Circle(f64),Rect(f64,f64),Empty}"),
            "enum Shape { Circle(f64), Rect(f64, f64), Empty }\n");
}

TEST(Tooling_Fmt, MatchDestructuringPattern) {
  std::string once =
      fmt("function area(s: ref Shape) f64 {\n"
          "return match s {\n"
          "Shape.Circle(r) => 3.0 * r * r,\n"
          "Shape.Rect(w, _) => w,\n"
          "_ => 0.0\n"
          "};\n"
          "}");
  // Idempotency: formatting the formatted output is a fixed point
  EXPECT_EQ(fmt(once), once);
  EXPECT_NE(once.find("Shape.Circle(r) => "), std::string::npos);
  EXPECT_NE(once.find("Shape.Rect(w, _) => "), std::string::npos);
}

TEST(Tooling_Fmt, GenericEnumDeclaration) {
  EXPECT_EQ(fmt("enum Option<T>{Some(T),None}"),
            "enum Option<T> { Some(T), None }\n");
}

// ------------------------------------------------------------------
// Character and byte literals
// ------------------------------------------------------------------

// Literals are reprinted as a verbatim source slice, so every spelling --
// escapes, raw UTF-8, \u{...}, and the byte form -- survives untouched.
TEST(Tooling_Fmt, CharAndByteLiteralsRoundTrip) {
  const std::string src =
      "function main() i32 {\n"
      "  var a: char = 'a';\n"
      "  var b: char = '\\n';\n"
      "  var c: char = '\\u{1F600}';\n"
      "  var d: char = '\xC3\xA9';\n"
      "  var e: char = '\\'';\n"
      "  var f: u8 = b'\\xFF';\n"
      "  var g: u8 = b' ';\n"
      "  return 0;\n"
      "}\n";
  EXPECT_EQ(fmt(src), src);
  EXPECT_EQ(fmt(fmt(src)), src);  // idempotent
}
