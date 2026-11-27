// tests/tooling/fmt/test_fmt.cpp — sun fmt formatter tests

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cli/fmt_command.h"
#include "parsing/config_formatter.h"
#include "parsing/formatter.h"
#include "support/error.h"

/** Formats fixture source for comparison with the expected output. */
static std::string fmt(const std::string& src) {
  return sun::parsing::formatSource(src);
}

/** Formatting preserves a lambda's exception effect on repeated passes. */
TEST(Tooling_Fmt, ThrowingLambda) {
  const auto formatted = fmt("var work = () => void throws { fail(); };\n");
  EXPECT_NE(formatted.find("=> void throws"), std::string::npos);
  EXPECT_EQ(fmt(formatted), formatted);
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

/** Manifests use two spaces per level without interpreting literal delimiters.
 */
TEST(Tooling_Fmt, ManifestIndentation) {
  const std::string input =
      "manifest {\n"
      "    source_files: [\n"
      "        // Keep the } and ] in this comment.\n"
      "        \"a[{}].sun\",\n"
      "        \"other.sun\"\n"
      "    ],\n"
      "    target: {\n"
      "        macos: {\n"
      "            source_files: [\"darwin.sun\"]\n"
      "        }\n"
      "    }\n"
      "}\n";
  const std::string expected =
      "manifest {\n"
      "  source_files: [\n"
      "    // Keep the } and ] in this comment.\n"
      "    \"a[{}].sun\",\n"
      "    \"other.sun\"\n"
      "  ],\n"
      "  target: {\n"
      "    macos: {\n"
      "      source_files: [\n"
      "        \"darwin.sun\"\n"
      "      ]\n"
      "    }\n"
      "  }\n"
      "}\n";
  EXPECT_EQ(fmt(input), expected);
  EXPECT_EQ(fmt(expected), expected);
}

// A test function prints its keyword and nothing of its synthesized
// signature (void return, implicit throws), so the output re-parses.
TEST(Tooling_Fmt, TestFunctionKeepsItsKeyword) {
  EXPECT_EQ(fmt("test_function checksThings(){return;}"),
            "test_function checksThings() {\n"
            "  return;\n"
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
  EXPECT_EQ(fmt("function f(a: ref  Foo, b: raw_ptr<Bar>) Result<i32> {\n"
                "return Result.Error(1);\n"
                "}"),
            "function f(a: ref  Foo, b: raw_ptr<Bar>) Result<i32> {\n"
            "  return Result.Error(1);\n"
            "}\n");
}

// Constructors and destructors print bare — no 'public function', no return
// type — and a throwing constructor keeps its 'throws IError'.
TEST(Tooling_Fmt, LifecycleMethods) {
  EXPECT_EQ(fmt("class A {\n"
                "  var x: i32;\n"
                "  init(v: i32) throws {\n"
                "    this.x = v;\n"
                "  }\n"
                "\n"
                "  deinit() {\n"
                "    this.x = 0;\n"
                "  }\n"
                "}"),
            "class A {\n"
            "  var x: i32;\n"
            "\n"
            "  init(v: i32) throws {\n"
            "    this.x = v;\n"
            "  }\n"
            "\n"
            "  deinit() {\n"
            "    this.x = 0;\n"
            "  }\n"
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

/** Keeps empty modules compact and separates comment-only module contents once.
 */
TEST(Tooling_Fmt, ModuleOpeningBlankLine) {
  EXPECT_EQ(fmt("module empty {}"), "module empty {}\n");
  const std::string expected = "module notes {\n\n  /** Contents. */\n}\n";
  EXPECT_EQ(fmt("module notes {\n/** Contents. */\n}"), expected);
  EXPECT_EQ(fmt("module notes {\n\n\n/** Contents. */\n}"), expected);
  EXPECT_EQ(fmt(expected), expected);
}

TEST(Tooling_Fmt, BlankLineBetweenFunctions) {
  EXPECT_EQ(fmt("function a() void {}\n\nfunction b() void {}\n"),
            "function a() void {}\n"
            "\n"
            "function b() void {}\n");
}

/** Separates adjacent functions regardless of their original spacing. */
TEST(Tooling_Fmt, InsertsBlankLinesBetweenFunctions) {
  const std::string expected =
      "/** First. */\n"
      "function first() void {}\n"
      "\n"
      "/** Second. */\n"
      "public function second() void {}\n"
      "\n"
      "/** Check. */\n"
      "test_function check() {}\n";
  EXPECT_EQ(fmt("/** First. */\nfunction first() void {}\n"
                "/** Second. */\npublic function second() void {}\n"
                "/** Check. */\ntest_function check() {}"),
            expected);
  EXPECT_EQ(fmt(expected), expected);
}

/** Separates every pair of declaration kinds before attached documentation. */
TEST(Tooling_Fmt, BlankLinesBetweenDeclarationKinds) {
  const std::string first[] = {"function left() void {}", "class Left {}",
                               "interface Left {}", "enum Left { Value }",
                               "module left {}"};
  const std::string second[] = {"function right() void {}", "class Right {}",
                                "interface Right {}", "enum Right { Value }",
                                "module right {}"};
  for (const auto& left : first) {
    for (const auto& right : second) {
      SCOPED_TRACE(left + " -> " + right);
      const auto expected =
          fmt(left) + "\n/** Next declaration. */\n" + fmt(right);
      EXPECT_EQ(fmt(left + "\n/** Next declaration. */\n" + right), expected);
      EXPECT_EQ(fmt(expected), expected);
      EXPECT_EQ(fmt(left + "\n" + right), fmt(left) + "\n" + fmt(right));
    }
  }
}

/** Separates the XML-style enum and class without detaching their comments. */
TEST(Tooling_Fmt, DocumentedTypesInsideModule) {
  const std::string source =
      "public module std {\n"
      "  /** Supported content. */\n"
      "  public enum XmlNodeValue { Text(String) }\n"
      "  /** Owned content. */\n"
      "  public class XmlNode {\n  }\n"
      "}\n";
  auto expected = source;
  expected.insert(expected.find("  /** Supported content."), "\n");
  expected.insert(expected.find("  /** Owned content."), "\n");
  EXPECT_EQ(fmt(source), expected);
  EXPECT_EQ(fmt(expected), expected);
}

/** Keeps method documentation attached and avoids duplicate blank lines. */
TEST(Tooling_Fmt, InsertsBlankLinesBetweenDocumentedMethods) {
  const std::string source =
      "/** Readiness. */\nclass Event {\n"
      "  /** Construct. */\n  init() {} // initialized\n"
      "  /** Read. */\n  public const method read() bool { return true; }\n"
      "\n\n  /** Write. */\n  public const method write() bool { return false; "
      "}\n"
      "  /** Destroy. */\n  deinit() {}\n}\n";
  const std::string expected =
      "/** Readiness. */\nclass Event {\n"
      "  /** Construct. */\n  init() {}  // initialized\n"
      "\n  /** Read. */\n  public const method read() bool {\n"
      "    return true;\n  }\n"
      "\n  /** Write. */\n  public const method write() bool {\n"
      "    return false;\n  }\n"
      "\n  /** Destroy. */\n  deinit() {}\n}\n";
  EXPECT_EQ(fmt(source), expected);
  EXPECT_EQ(fmt(expected), expected);
}

/** Keeps field groups together and separates documented methods on both sides.
 */
TEST(Tooling_Fmt, BlankLinesBetweenFieldsAndMethods) {
  const std::string source =
      "class Node {\n"
      "  var value: i32;\n"
      "  var count: i32; // count\n"
      "  /** Construct. */\n"
      "  init() {}\n"
      "  /** Extra data. */\n"
      "  var extra: i32;\n"
      "  /** Read. */\n"
      "  method read() void {}\n"
      "}\n";
  auto expected = source;
  expected.replace(expected.find("; // count"), 10, ";  // count");
  expected.insert(expected.find("  /** Construct."), "\n");
  expected.insert(expected.find("  /** Extra data."), "\n");
  expected.insert(expected.find("  /** Read."), "\n");
  EXPECT_EQ(fmt(source), expected);
  EXPECT_EQ(fmt(expected), expected);
}

/** Applies field-to-method separation to interface members too. */
TEST(Tooling_Fmt, BlankLinesBetweenInterfaceFieldsAndMethods) {
  const std::string source =
      "interface View {\n"
      "  var first: i32;\n"
      "  var second: i32;\n"
      "  /** Read. */\n"
      "  method read() i32;\n"
      "}\n";
  auto expected = source;
  expected.insert(expected.find("  /** Read."), "\n");
  EXPECT_EQ(fmt(source), expected);
  EXPECT_EQ(fmt(expected), expected);
}

/** Applies the same spacing inside modules and interface declarations. */
TEST(Tooling_Fmt, FunctionSpacingInModulesAndInterfaces) {
  const std::string source =
      "/** Helpers. */\nmodule helpers {\n"
      "  /** First. */\n  function first() void {}\n"
      "  /** Second. */\n  function second() void {}\n}\n"
      "/** Reader. */\ninterface Reader {\n"
      "  /** Read. */\n  method read() i32;\n"
      "  /** Peek. */\n  method peek() i32;\n}\n";
  auto expected = source;
  expected.insert(expected.find("  /** First."), "\n");
  expected.insert(expected.find("  /** Second."), "\n");
  expected.insert(expected.find("  /** Peek."), "\n");
  expected.insert(expected.find("/** Reader."), "\n");
  EXPECT_EQ(fmt(source), expected);
  EXPECT_EQ(fmt(expected), expected);
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
            "\n"
            "function f() void {\n"
            "  g(1, 2);\n"
            "}\n");
}

TEST(Tooling_Fmt, MultiLineCallStaysMultiLine) {
  EXPECT_EQ(fmt("function g(a: i32, b: i32) void {}\n"
                "function f() void {\ng(1,\n2);\n}\n"),
            "function g(a: i32, b: i32) void {}\n"
            "\n"
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
  EXPECT_THROW(fmt("function f( {"), sun::support::SunError);
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
                "init(start: i32) { this.count = start; }\n"
                "\n"
                "method get() i32 { return this.count; }\n"
                "}"),
            "class Counter {\n"
            "  var count: i32;\n"
            "\n"
            "  init(start: i32) {\n"
            "    this.count = start;\n"
            "  }\n"
            "\n"
            "  method get() i32 {\n"
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
  EXPECT_EQ(fmt("partial class X {\nmethod m() void {}\n}"),
            "partial class X {\n"
            "  method m() void {}\n"
            "}\n");
}

TEST(Tooling_Fmt, InterfaceSignatureAndDefault) {
  EXPECT_EQ(fmt("interface IAllocator {\n"
                "method alloc(size: i64) raw_ptr<u8>;\n"
                "method zero() i32 { return 0; }\n"
                "}"),
            "interface IAllocator {\n"
            "  method alloc(size: i64) raw_ptr<u8>;\n"
            "\n"
            "  method zero() i32 {\n"
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
  EXPECT_EQ(fmt("function f(x:i32) i32 throws{return div(10,x);}"),
            "function f(x: i32) i32 throws {\n"
            "  return div(10, x);\n"
            "}\n");
}

TEST(Tooling_Fmt, DottedModule) {
  EXPECT_EQ(fmt("module std.io {\nfunction f() void {}\n}"),
            "module std.io {\n\n"
            "  function f() void {}\n"
            "}\n");
}

TEST(Tooling_Fmt, NestedModulesStayNested) {
  EXPECT_EQ(fmt("module a {\nmodule b {\nfunction f() void {}\n}\n}"),
            "module a {\n\n"
            "  module b {\n\n"
            "    function f() void {}\n"
            "  }\n"
            "}\n");
}

/** Separates imports from documented declarations while keeping imports
 * together. */
TEST(Tooling_Fmt, UsingBeforeDeclaration) {
  for (const std::string declaration :
       {"function draw() void {}", "class Drawing {}", "interface Shape {}",
        "enum Color { Red }", "module drawing {}"}) {
    const std::string imports = "using std;\nusing std.Vec;\n";
    const std::string comment = "/** Draws a shape. */\n";
    const std::string input = imports + comment + declaration;
    const std::string expected = imports + "\n" + comment + fmt(declaration);
    EXPECT_EQ(fmt(input), expected);
    EXPECT_EQ(fmt(expected), expected);
    const std::string nested = fmt("module example {\n" + input + "\n}");
    EXPECT_NE(nested.find("  using std.Vec;\n\n  /** Draws a shape. */"),
              std::string::npos);
    EXPECT_EQ(fmt(nested), nested);
    EXPECT_EQ(fmt(imports + declaration), imports + "\n" + fmt(declaration));
  }
}

TEST(Tooling_Fmt, UsingAndDeclare) {
  EXPECT_EQ(fmt("using std;\n"
                "using std.Vec;\n"
                "declare Vec_i32 = Vec<i32>;\n"
                "declare function isOdd(n: i32) bool;\n"
                "\n"
                "extern function _malloc(size: i64) raw_ptr<u8>;\n"),
            "using std;\n"
            "using std.Vec;\n"
            "declare Vec_i32 = Vec<i32>;\n"
            "declare function isOdd(n: i32) bool;\n"
            "\n"
            "extern function _malloc(size: i64) raw_ptr<u8>;\n");
}

TEST(Tooling_Fmt, LambdaAndCaptures) {
  EXPECT_EQ(fmt("function f() i32 {\n"
                "var add = (x: i32) => i32 { return x + 3; };\n"
                "var n: i32 = 1;\n"
                "var bump = [ref n]() => void { n += 1; };\n"
                "return add(1);\n"
                "}"),
            "function f() i32 {\n"
            "  var add = (x: i32) => i32 { return x + 3; };\n"
            "  var n: i32 = 1;\n"
            "  var bump = [ref n]() => void { n += 1; };\n"
            "  return add(1);\n"
            "}\n");
}

TEST(Tooling_Fmt, FunctionPointerType) {
  EXPECT_EQ(fmt("function apply(cb:function(i32,bool)Result<i64>)void{}"),
            "function apply(cb: function (i32, bool) Result<i64>) void {}\n");
  EXPECT_EQ(fmt("function choose()function(i32)i32{return double;}"),
            "function choose() function (i32) i32 {\n"
            "  return double;\n"
            "}\n");
}

TEST(Tooling_Fmt, LambdaLifetimeParametersBeforeCaptures) {
  EXPECT_EQ(fmt("function f() void {\n"
                "var n=0;\n"
                "var store=<'a> [ref n](cb: <'a>() => i32, box: ref 'a Box) => "
                "void { n += 1; return; };\n"
                "return;\n"
                "}"),
            "function f() void {\n"
            "  var n = 0;\n"
            "  var store = <'a> [ref n](cb: <'a>() => i32, box: ref 'a "
            "Box) => void { n += 1; return; };\n"
            "  return;\n"
            "}\n");
}

TEST(Tooling_Fmt, LambdaCanonicalPrefixes) {
  EXPECT_EQ(
      fmt("function f() void {\n"
          "var n=1;\n"
          "var plain=()=>i32{return 1;};\n"
          "var owned=[n]()=>i32{return n;};\n"
          "var shared=[const ref n]()=>i32{return n;};\n"
          "var lifetime=<'a>(cb: <'a>() => i32)=><'a>() => i32{return cb;};\n"
          "return;\n"
          "}"),
      "function f() void {\n"
      "  var n = 1;\n"
      "  var plain = () => i32 { return 1; };\n"
      "  var owned = [n]() => i32 { return n; };\n"
      "  var shared = [const ref n]() => i32 { return n; };\n"
      "  var lifetime = <'a>(cb: <'a>() => i32) => <'a>() => i32 { return "
      "cb; };\n"
      "  return;\n"
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
                "public const method get() i32 { return this.n; }\n"
                "const method zero() bool { return this.n == 0; }\n"
                "}\n"
                "\ninterface I {\n"
                "const method get() i32;\n"
                "}\n"
                "\nfunction f(v: ref Vec<i32>) void {\n"
                "for (const x: i32 in v) { }\n"
                "}"),
            "class C {\n"
            "  var n: i32;\n\n"
            "  public const method get() i32 {\n"
            "    return this.n;\n"
            "  }\n"
            "\n"
            "  const method zero() bool {\n"
            "    return this.n == 0;\n"
            "  }\n"
            "}\n"
            "\ninterface I {\n"
            "  const method get() i32;\n"
            "}\n"
            "\nfunction f(v: ref Vec<i32>) void {\n"
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
#include "serialization/source_file_ids.h"

using sun::ast::BlockExprAST;

/** Keeps test fixtures and helpers local to this source file. */
namespace {

/** Lists the source files included in the formatter regression corpus. */
std::vector<std::filesystem::path> corpusFiles() {
  std::vector<std::filesystem::path> files;
  for (const char* dir : {"tests/programs", "stdlib", "examples"}) {
    if (!std::filesystem::exists(dir)) {
      continue;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.path().extension() == ".sun") {
        files.push_back(entry.path());
      }
    }
  }
  // Namespace directories replace the former flat stdlib source layout.
  if (std::filesystem::exists("stdlib/std")) {
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator("stdlib/std")) {
      if (entry.path().extension() == ".sun" &&
          entry.path().parent_path().filename() != "bindings") {
        files.push_back(entry.path());
      }
    }
  }
  return files;
}

/** Reads a fixture file into a string for comparison. */
std::string readFile(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/**
 * Post-lowering, position-free serialization for structural comparison.
 * Mutates the tree (lowering runs in place); use only when done with it.
 */
std::string loweredFingerprint(BlockExprAST& ast) {
  // Normalize only the global ordering that formatting deliberately changes.
  auto& body = ast.mutableBody();
  /** Places imports first and manifests last without changing other nodes. */
  const auto rank = [](const auto& node) {
    if (node->getType() == sun::ast::ASTNodeType::USING) {
      return 0;
    }
    if (node->getType() == sun::ast::ASTNodeType::MANIFEST) {
      return 2;
    }
    return 1;
  };
  std::stable_sort(body.begin(), body.end(), [&](const auto& a, const auto& b) {
    if (rank(a) != rank(b)) {
      return rank(a) < rank(b);
    }
    return rank(a) == 0 && a->toString() < b->toString();
  });
  sun::parsing::LoweringPass lowering;
  lowering.run(ast);
  sun::serialization::SerializerConfig config;
  config.include_location = false;
  sun::serialization::ASTSerializer serializer(config);
  auto program = serializer.serializeProgram(ast);
  // Each corpus tree represents one file; its allocated identity can differ.
  sun::serialization::remapSourceFiles(program, [](sun::support::SourceFileId) {
    return sun::support::SourceFileId{1};
  });
  return program.SerializeAsString();
}

}  // namespace

TEST(Tooling_Fmt_Corpus, IdempotentAndStructurePreserving) {
  /**
   * One shared parser; each corpus file is parsed exactly twice (original and
   * formatted)
   */
  std::istringstream dummy("");
  sun::parsing::Parser parser(dummy);
  parser.setCollectComments(true);

  int checked = 0;
  for (const auto& path : corpusFiles()) {
    std::string source = readFile(path);
    std::unique_ptr<BlockExprAST> ast;
    try {
      ast = parser.parseString(source);
    } catch (const sun::support::SunError&) {
      continue;  // legacy files (import statements) don't parse standalone
    }
    if (!ast) {
      continue;
    }
    ++checked;

    std::string formatted =
        sun::parsing::formatProgram(*ast, parser.getComments(), source);
    std::string originalFp = loweredFingerprint(*ast);

    // Re-parses cleanly and is idempotent
    std::unique_ptr<BlockExprAST> ast2;
    ASSERT_NO_THROW(ast2 = parser.parseString(formatted)) << path;
    ASSERT_NE(ast2, nullptr) << path;
    std::string reformatted =
        sun::parsing::formatProgram(*ast2, parser.getComments(), formatted);
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

// Regression (#200): a trailing comment on the `match x {` line moves inside
// the block; a preceding statement must not then earn it a blank line on the
// second pass.
TEST(Tooling_Fmt, MatchOpeningLineTrailingCommentIsIdempotent) {
  std::string once =
      fmt("function classify(o: Option<i64>) i64 {\n"
          "var y = 1;\n"
          "var idx = match o {  // trailing comment here\n"
          "Option.Some(i) => i,\n"
          "Option.None => -1\n"
          "};\n"
          "return idx;\n"
          "}\n");
  EXPECT_EQ(once,
            "function classify(o: Option<i64>) i64 {\n"
            "  var y = 1;\n"
            "  var idx = match o {\n"
            "    // trailing comment here\n"
            "    Option.Some(i) => i,\n"
            "    Option.None => -1\n"
            "  };\n"
            "  return idx;\n"
            "}\n");
  EXPECT_EQ(fmt(once), once);
}

// Same shape for a multiline enum header
TEST(Tooling_Fmt, EnumOpeningLineTrailingCommentIsIdempotent) {
  std::string once =
      fmt("function f() void {}\n\n"
          "enum Shape {  // shapes\n"
          "Circle,\n"
          "Rect\n"
          "}\n");
  EXPECT_EQ(once,
            "function f() void {}\n\n"
            "enum Shape {\n"
            "  // shapes\n"
            "  Circle,\n"
            "  Rect\n"
            "}\n");
  EXPECT_EQ(fmt(once), once);
}

TEST(Tooling_Fmt, GenericEnumDeclaration) {
  EXPECT_EQ(fmt("enum Option<T>{Some(T),None}"),
            "enum Option<T> { Some(T), None }\n");
}

// A constraint dropped here would silently rewrite working code into code
// that no longer compiles, so every declaration form round-trips it.
TEST(Tooling_Fmt, TypeParameterConstraintOnFunction) {
  EXPECT_EQ(fmt("function twice<T:_Numeric>(x:T)T{return x+x;}"),
            "function twice<T: _Numeric>(x: T) T {\n"
            "  return x + x;\n"
            "}\n");
}

TEST(Tooling_Fmt, TypeParameterConstraintLambda) {
  EXPECT_EQ(fmt("function run<F:_Lambda>(f:F)i32{return 0;}"),
            "function run<F: _Lambda>(f: F) i32 {\n"
            "  return 0;\n"
            "}\n");
}

TEST(Tooling_Fmt, TypeParameterConstraintMixedWithPlainParams) {
  EXPECT_EQ(fmt("function pair<T,U:_Numeric>(a:T,b:U)i32{return 0;}"),
            "function pair<T, U: _Numeric>(a: T, b: U) i32 {\n"
            "  return 0;\n"
            "}\n");
}

TEST(Tooling_Fmt, TypeParameterConstraintOnClass) {
  EXPECT_EQ(fmt("class Box<T:_Numeric>{var v:T;}"),
            "class Box<T: _Numeric> {\n"
            "  var v: T;\n"
            "}\n");
}

TEST(Tooling_Fmt, TypeParameterConstraintOnEnum) {
  EXPECT_EQ(fmt("enum Maybe<T:_Numeric>{Some(T),None}"),
            "enum Maybe<T: _Numeric> { Some(T), None }\n");
}

TEST(Tooling_Fmt, TypeParameterConstraintOnInterface) {
  EXPECT_EQ(fmt("interface IBox<T:_Numeric>{public method get()T;}"),
            "interface IBox<T: _Numeric> {\n"
            "  public method get() T;\n"
            "}\n");
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

TEST(Tooling_Fmt, ExternGlobalsPreserveAbiVisibilityAndLinkName) {
  EXPECT_EQ(fmt("public extern \"C\" var value:raw_ptr<i32> as \"C_name\";"),
            "public extern \"C\" var value: raw_ptr<i32> as \"C_name\";\n");
  EXPECT_EQ(fmt("extern var value:i32;"), "extern var value: i32;\n");
}

TEST(Tooling_Fmt, ClassFieldInitializers) {
  const std::string source =
      "class Foo{var x:i32=10;var y:i32=this.x+2;init(){}}";
  const std::string expected =
      "class Foo {\n"
      "  var x: i32 = 10;\n"
      "  var y: i32 = this.x + 2;\n"
      "\n"
      "  init() {}\n"
      "}\n";
  EXPECT_EQ(fmt(source), expected);
  EXPECT_EQ(fmt(expected), expected);
}

TEST(Tooling_Fmt, IntegerBasesKeepSpelling) {
  EXPECT_EQ(fmt("var hex:u32=0x00AB_cdefu32;var bits=0b1010_0011;var n=1_000;"),
            "var hex: u32 = 0x00AB_cdefu32;\n"
            "var bits = 0b1010_0011;\n"
            "var n = 1_000;\n");
}

TEST(Tooling_Fmt, ExplicitEnumValues) {
  const std::string expected =
      "enum Kind { Data = 21, Next, Negative = -2, Last }\n";
  EXPECT_EQ(fmt("enum Kind{Data=21,Next,Negative=-2,Last}"), expected);
  EXPECT_EQ(fmt(expected), expected);
}

TEST(Tooling_Fmt, EnumUnderlyingTypeAndUnsignedMaximum) {
  const std::string expected =
      "enum E u64 { High = 18446744073709551615, Low = 0 }\n";
  EXPECT_EQ(fmt("enum E u64{High=18446744073709551615,Low=0}"), expected);
  EXPECT_EQ(fmt(expected), expected);
  EXPECT_EQ(fmt("enum E i8{Negative=-128,Next}"),
            "enum E i8 { Negative = -128, Next }\n");
}

TEST(Tooling_Fmt, UnsafeMethodsAndCallableTypes) {
  const std::string source = R"(
interface IRead { public const unsafe method read() i32; }
class Reader {
  public const unsafe method read() i32 { return 42; }
}
function invoke(f: unsafe <'_>() => i32) i32 {
  return unsafe { f(); };
}
)";
  const auto formatted = fmt(source);
  EXPECT_NE(formatted.find("public const unsafe method read() i32"),
            std::string::npos);
  EXPECT_NE(formatted.find("unsafe <'_>() => i32"), std::string::npos);
  EXPECT_EQ(fmt(formatted), formatted);
}

TEST(Tooling_Fmt, UnsafeExpressionsPreserveScope) {
  const std::string source = R"(function f() i32 {
  unsafe write(unsafe read());
  var a = unsafe read() + other();
  var b = unsafe (read() + other());
  var c = (unsafe read()).next();
  return unsafe -items[0];
}
)";
  EXPECT_EQ(fmt(source), source);
  EXPECT_EQ(fmt(fmt(source)), source);
}

TEST(Tooling_Fmt, UnsafeExpressionKeepsComments) {
  const std::string source = R"(function f() i32 {
  return unsafe /* caller checked the index */ read();
}
)";
  const auto formatted = fmt(source);
  EXPECT_NE(formatted.find("/* caller checked the index */"),
            std::string::npos);
  EXPECT_EQ(fmt(formatted), formatted);
}

TEST(Tooling_Fmt, GenericInterfaceConstraint) {
  const auto formatted = fmt("class Server<H:IHandler<H> >{var handler:H;}");
  EXPECT_NE(formatted.find("H: IHandler<H>"), std::string::npos);
  EXPECT_EQ(fmt(formatted), formatted);
}

TEST(Tooling_Fmt, GenericConstraintKeepsTypeArgumentSyntax) {
  const auto formatted = fmt(
      "function read<H:IValue<raw_ptr<i32>, const ref H>>(item:ref H) void {}");
  EXPECT_NE(formatted.find("IValue<raw_ptr<i32>, const ref H>"),
            std::string::npos);
  EXPECT_EQ(fmt(formatted), formatted);
}

TEST(Tooling_Fmt, DocumentedPublicDottedModule) {
  const std::string expected =
      "/** Readers. */\n"
      "public module a.b.c {\n\n"
      "  // Reads a value.\n"
      "  public function read() i32 {\n"
      "    return 1;\n"
      "  }\n"
      "}\n";
  EXPECT_EQ(fmt("/** Readers. */\npublic module a.b.c{\n"
                "// Reads a value.\npublic function read()i32{return 1;}\n}"),
            expected);
  EXPECT_EQ(fmt(expected), expected);
}

TEST(Tooling_Fmt, PublicNestedModulesStayNested) {
  const std::string source =
      "// Outer.\npublic module a {\n\n"
      "  // Inner.\n  public module b {\n\n"
      "    function f() void {}\n  }\n}\n";
  EXPECT_EQ(fmt(source), source);
  EXPECT_EQ(fmt(fmt(source)), source);
}

/** Keeps the single qualified parent when formatting and reformatting. */
TEST(Tooling_Fmt, InterfaceParent) {
  auto formatted =
      fmt("interface Child<T> extends lib.Parent<T>{method read()T;}");
  EXPECT_NE(formatted.find("interface Child<T> extends lib.Parent<T> {"),
            std::string::npos);
  EXPECT_EQ(fmt(formatted), formatted);
}

/** Preserves alignment metadata and keeps its spelling stable across
 * formatting. */
TEST(Tooling_Fmt, ClassAlignment) {
  auto formatted = fmt("class Vector {alignment:16 var x:f32;}");
  EXPECT_EQ(formatted, "class Vector {\n  alignment: 16\n  var x: f32;\n}\n");
  EXPECT_EQ(fmt(formatted), formatted);
}

/** Exercises project exclusions through the formatter command without changing
 * cwd. */
class Tooling_FmtIgnore : public ::testing::Test {
 protected:
  std::filesystem::path dir;

  /** Creates an isolated project inside the workspace temporary directory. */
  void SetUp() override {
    dir = std::filesystem::current_path() / "tmp" / "fmt_ignore" /
          ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(dir);
    write("sun-config.json", R"({"root":true})");
  }

  /** Removes the temporary project after each test. */
  void TearDown() override { std::filesystem::remove_all(dir); }

  /** Writes a fixture relative to the isolated project. */
  void write(const std::string& path, const std::string& text) {
    std::filesystem::create_directories((dir / path).parent_path());
    std::ofstream(dir / path)
        << (std::filesystem::path(path).filename() == "sun-config.json"
                ? sun::parsing::formatConfig(text)
                : text);
  }

  /** Reads a fixture so tests can verify which files were changed. */
  std::string read(const std::string& path) {
    std::ifstream input(dir / path);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }

  /** Runs check mode on one project path with an optional override. */
  int check(const std::string& path, bool noIgnore = false) {
    std::vector<std::string> args = {"--check", (dir / path).string()};
    if (noIgnore) {
      args.push_back("--no-ignore");
    }
    return sun::cli::runFmtCommand(args);
  }
};

/** Excluded directories are pruned and explicitly named files respect
 * exclusions. */
TEST_F(Tooling_FmtIgnore, DirectoriesAndOverride) {
  write("sun-config.json", R"({"root":true,"sun_fmt":{"ignore":["/build/"]}})");
  write("build/bad.sun", "not valid Sun!");
  write("main.sun", "function f() void {}\n");
  EXPECT_EQ(check("."), 0);
  EXPECT_EQ(check("build"), 0);
  EXPECT_EQ(check("build/bad.sun"), 0);
  EXPECT_EQ(check("build/bad.sun", true), 2);
  write("build/bad.sun", "function f()void{}");
  EXPECT_EQ(check("build/bad.sun", true), 1);
  EXPECT_EQ(sun::cli::runFmtCommand({(dir / "build/bad.sun").string()}), 0);
  EXPECT_EQ(read("build/bad.sun"), "function f()void{}");
  EXPECT_EQ(sun::cli::runFmtCommand(
                {"--no-ignore", (dir / "build/bad.sun").string()}),
            0);
  EXPECT_EQ(read("build/bad.sun"), "function f() void {}\n");
}

/** Patterns support anchoring, directory components, wildcards, and ordered
 * exceptions. */
TEST_F(Tooling_FmtIgnore, PatternsAndNegation) {
  write("sun-config.json", R"({"root":true,"sun_fmt":{"ignore":[
    "/root.sun", "cache/", "*.generated.sun", "src/**/skip?.sun",
    "/output/**", "!/output/keep.sun", "[ab].sun", "\\!literal.sun"
  ]}})");
  for (const auto* path :
       {"root.sun", "nested/root.sun", "cache/bad.sun", "nested/cache/bad.sun",
        "nested/x.generated.sun", "src/skip1.sun", "src/a/b/skip2.sun",
        "src/skip12.sun", "output/drop.sun", "output/keep.sun", "a.sun",
        "!literal.sun"}) {
    write(path, "function f()void{}");
  }
  for (const auto* path :
       {"root.sun", "cache/bad.sun", "nested/cache/bad.sun",
        "nested/x.generated.sun", "src/skip1.sun", "src/a/b/skip2.sun",
        "output/drop.sun", "a.sun", "!literal.sun"}) {
    EXPECT_EQ(check(path), 0) << path;
  }
  for (const auto* path :
       {"nested/root.sun", "src/skip12.sun", "output/keep.sun"}) {
    EXPECT_EQ(check(path), 1) << path;
  }
  EXPECT_EQ(check("output"), 1);
}

/** Nearer configurations override ancestors and root boundaries stop
 * inheritance. */
TEST_F(Tooling_FmtIgnore, NestedConfiguration) {
  write("sun-config.json", R"({"root":true,"sun_fmt":{"ignore":["*.sun"]}})");
  write("nested/sun-config.json", R"({"sun_fmt":{"ignore":["!keep.sun"]}})");
  write("nested/keep.sun", "function f()void{}");
  write("nested/drop.sun", "function f()void{}");
  EXPECT_EQ(check("nested/keep.sun"), 1);
  EXPECT_EQ(check("nested/drop.sun"), 0);
  EXPECT_EQ(check("nested"), 1);
  write("nested/sun-config.json", R"({"root":true,"sun_fmt":{"ignore":[]}})");
  EXPECT_EQ(check("nested/drop.sun"), 1);
}

/** A file exception cannot reopen an excluded directory. */
TEST_F(Tooling_FmtIgnore, ExcludedParents) {
  write("sun-config.json",
        R"({"root":true,"sun_fmt":{"ignore":["cache/","!cache/keep.sun"]}})");
  write("cache/keep.sun", "function f()void{}");
  EXPECT_EQ(check("."), 0);
  EXPECT_EQ(check("cache/keep.sun"), 0);
}

/** Invalid configuration produces an error before any files are rewritten. */
TEST_F(Tooling_FmtIgnore, InvalidConfiguration) {
  write("main.sun", "function f()void{}");
  for (const auto* config : {R"({"root":true,"sun_fmt":[]})",
                             R"({"root":true,"sun_fmt":{"ignore":"build"}})",
                             R"({"root":true,"sun_fmt":{"ignore":[42]}})",
                             R"({"root":true,"sun_fmt":{"ignored":[]}})",
                             R"({"root":true,"sun_fmt":{"ignore":["["]}})"}) {
    write("sun-config.json", config);
    EXPECT_EQ(check("main.sun"), 2) << config;
    EXPECT_EQ(read("main.sun"), "function f()void{}");
  }
  EXPECT_EQ(check("main.sun", true), 1);
}

/** Existing hidden-directory and template exclusions survive --no-ignore. */
TEST_F(Tooling_FmtIgnore, BuiltInExclusions) {
  write(".hidden/bad.sun", "not valid Sun!");
  write("template.inja.sun", "not valid Sun!");
  write("main.sun", "function f() void {}\n");
  EXPECT_EQ(check(".", true), 0);
}

/** Manifest strings receive separate lines while comments stay with entries. */
TEST(Tooling_Fmt, ManifestListsAlwaysExpand) {
  const std::string input = R"(manifest {
  libraries: ["stdlib.moon", "sun_ui.moon", "sun_gobject.moon", "sun_gtk.moon", "sun_ui_gtk.moon"]
  source_files: ["process.sun", /* keep [,] */ "media.sun", "job.sun"]
  test_files: []
}
)";
  const std::string expected = R"(manifest {
  source_files: [
    "process.sun", /* keep [,] */
    "media.sun",
    "job.sun"
  ]
  test_files: []
  libraries: [
    "stdlib.moon",
    "sun_ui.moon",
    "sun_gobject.moon",
    "sun_gtk.moon",
    "sun_ui_gtk.moon"
  ]
}
)";
  EXPECT_EQ(fmt(input), expected);
  EXPECT_EQ(fmt(expected), expected);
}

/** Long parameter, argument, array, and field lists wrap and remain stable. */
TEST(Tooling_Fmt, WrapsLongSyntaxAtOneHundredColumns) {
  const std::string source = R"(
function process(first_parameter_name: i32, second_parameter_name: i32, third_parameter_name: i32, fourth_parameter_name: i32) i32 {
  var values = [first_parameter_name, second_parameter_name, third_parameter_name, fourth_parameter_name];
  var record = { first_field: first_parameter_name, second_field: second_parameter_name, third_field: third_parameter_name };
  return process(first_parameter_name, second_parameter_name, third_parameter_name, fourth_parameter_name);
}
)";
  const auto formatted = fmt(source);
  EXPECT_NE(formatted.find("function process(\n"), std::string::npos);
  EXPECT_NE(formatted.find("var values = [\n"), std::string::npos);
  EXPECT_NE(formatted.find("var record = {\n"), std::string::npos);
  EXPECT_NE(formatted.find("return process(\n"), std::string::npos);
  std::istringstream lines(formatted);
  for (std::string line; std::getline(lines, line);) {
    EXPECT_LE(line.size(), 100u) << line;
  }
  EXPECT_EQ(fmt(formatted), formatted);
}

/** Wrapping leaves indivisible literal contents unchanged. */
TEST(Tooling_Fmt, PreservesLongLiteralContents) {
  const std::string literal = "\"" + std::string(150, 'x') + "\"";
  const auto formatted =
      fmt("function f() void { var text = " + literal + "; }");
  EXPECT_NE(formatted.find(literal), std::string::npos);
  EXPECT_EQ(fmt(formatted), formatted);
}

/** Nested calls and operator continuations keep their layout on later runs. */
TEST(Tooling_Fmt, WrappedNestedExpressionsAreIdempotent) {
  const auto formatted = fmt(R"(
function f() i32 {
  var callback = make_callback(first_long_argument_name, second_long_argument_name, third_long_argument_name)(1, 2);
  return first_long_argument_name + second_long_argument_name + third_long_argument_name + fourth_long_argument_name;
}
)");
  EXPECT_NE(formatted.find(")(\n"), std::string::npos);
  EXPECT_NE(formatted.find("+\n"), std::string::npos);
  EXPECT_EQ(fmt(formatted), formatted);
  std::istringstream lines(formatted);
  for (std::string line; std::getline(lines, line);) {
    EXPECT_LE(line.size(), 100u) << line;
  }
}

/** Inline manifest blocks expand around their fields and nested target blocks.
 */
TEST(Tooling_Fmt, ExpandsInlineManifestBraces) {
  const std::string expected = R"(manifest {
  libraries: [
    "stdlib.moon",
    "cuda.moon"
  ]
}
)";
  EXPECT_EQ(fmt(R"(manifest { libraries: ["stdlib.moon", "cuda.moon"] })"),
            expected);
  EXPECT_EQ(fmt(R"(manifest { libraries: [
    "stdlib.moon",
    "cuda.moon"
  ] })"),
            expected);
  EXPECT_EQ(fmt(expected), expected);
  const auto nested =
      fmt(R"(manifest { target: { linux: { libraries: ["stdlib.moon"] } } })");
  EXPECT_EQ(nested, R"(manifest {
  target: {
    linux: {
      libraries: [
        "stdlib.moon"
      ]
    }
  }
}
)");
  EXPECT_EQ(fmt(nested), nested);
}

/** Global imports move with documentation; nested imports retain their order.
 */
TEST(Tooling_Fmt, OrdersGlobalDeclarations) {
  const std::string source = R"(// File header.

manifest { libraries: ["std.moon"] }
/** Runs the program. */
function main() void {}
/** Imports z. */
using z; // z import
using a;
using a;
/** Contains local imports. */
module local {
  using z;
  using a;
}
// Footer.
)";
  const auto result = fmt(source);
  EXPECT_EQ(result.find("// File header.\n\nusing a;\nusing a;\n"), 0u);
  EXPECT_NE(result.find("/** Imports z. */\nusing z;  // z import\n\n/** Runs"),
            std::string::npos);
  EXPECT_NE(result.find("  using z;\n  using a;"), std::string::npos);
  EXPECT_NE(result.find("}\n\nmanifest {"), std::string::npos);
  EXPECT_EQ(result.substr(result.size() - 13), "}\n// Footer.\n");
  EXPECT_EQ(fmt(result), result);
}

/** Manifest sections and target sections share an order without sorting lists.
 */
TEST(Tooling_Fmt, OrdersManifestFields) {
  const std::string source = R"(manifest {
  target: { linux: { libraries: ["z.moon"] source_files: ["z.sun"] } }
  archives: ["z.a"]
  protos: ["z.proto"]
  // Libraries.
  libraries: ["z.moon", "a.moon"] // Keep with libraries.
  test_files: ["test.sun"]
  /** Sources. */
  source_files: ["z.sun", "a.sun"]
  // End of manifest.
}
)";
  const auto result = fmt(source);
  size_t previous = 0;
  for (const auto* field : {"source_files:", "test_files:", "libraries:",
                            "protos:", "archives:", "target:"}) {
    const auto position = result.find(field);
    EXPECT_GT(position, previous) << field;
    previous = position;
  }
  EXPECT_NE(result.find("// Libraries.\n  libraries:"), std::string::npos);
  EXPECT_NE(result.find("] // Keep with libraries.\n  protos:"),
            std::string::npos);
  EXPECT_LT(result.find("\"z.sun\""), result.find("\"a.sun\""));
  const auto target = result.substr(result.find("target:"));
  EXPECT_LT(target.find("source_files:"), target.find("libraries:"));
  EXPECT_EQ(fmt(result), result);
}

/** Manifest-only files do not gain a leading blank line. */
TEST(Tooling_Fmt, ManifestDocumentationSpacing) {
  EXPECT_EQ(fmt("/** Dependencies. */\nmanifest {}\nfunction f() void {}"),
            "function f() void {}\n\n/** Dependencies. */\nmanifest {}\n");
  EXPECT_EQ(fmt("manifest {}"), "manifest {}\n");
}

/** Root schema fields are ordered while unknown fields remain intact. */
TEST(Tooling_FmtConfig, RootOrder) {
  const auto result = sun::parsing::formatConfig(R"({
    "steps": [], "dependencies": {}, "entrypoints": [], "root": true,
    "package_id": "example/library", "custom_z": 1, "custom_a": 2
  })");
  EXPECT_EQ(
      result,
      "{\n  \"package_id\": \"example/library\",\n  \"root\": true,\n"
      "  \"entrypoints\": [],\n  \"dependencies\": {},\n  \"steps\": [],\n"
      "  \"custom_z\": 1,\n  \"custom_a\": 2\n}\n");
  EXPECT_EQ(sun::parsing::formatConfig(result), result);
}

/** Nested schemas and target alternatives retain user map and array ordering.
 */
TEST(Tooling_FmtConfig, NestedOrder) {
  const auto result = sun::parsing::formatConfig(R"({
    "steps": [{"environment":{"Z":"z","A":"a"},"outputs":["z","a"],
               "command":["tool","--flag"],"enabled":true,"name":"generate"}],
    "entrypoints": [{"resources":[{"destination":"out","path":"input"}],
      "path":{"target":{"linux":"z.sun","macos":"a.sun"},"default":"main.sun"},
      "type":"binary","name":"app"}],
    "dependencies": {"z":{"sha256":"hash","path":"z.moon","package":"x/z"},
                     "a":{"target":{"linux":{"version":"v1","git":"repo"}},
                          "default":{"version":"v2","git":"repo"}}}
  })");
  EXPECT_LT(result.find("\"name\": \"app\""), result.find("\"type\""));
  EXPECT_LT(result.find("\"default\""), result.find("\"target\""));
  EXPECT_LT(result.find("\"path\": \"input\""), result.find("\"destination\""));
  EXPECT_LT(result.find("\"z\": {"), result.find("\"a\": {"));
  EXPECT_LT(result.find("\"package\""), result.find("\"sha256\""));
  EXPECT_LT(result.find("\"git\""), result.find("\"version\""));
  EXPECT_LT(result.find("\"enabled\""), result.find("\"command\""));
  EXPECT_LT(result.find("\"Z\""), result.find("\"A\""));
  EXPECT_LT(result.find("\"tool\""), result.find("\"--flag\""));
  EXPECT_EQ(sun::parsing::formatConfig(result), result);
}

/** Invalid JSON and duplicate keys cannot silently lose configuration data. */
TEST(Tooling_FmtConfig, RejectsInvalidInput) {
  for (const auto* source :
       {"{", "[]", "null", "{\"root\":true,\"root\":false}",
        "{\"x\":{\"a\":1,\"a\":2}}"}) {
    SCOPED_TRACE(source);
    EXPECT_THROW(sun::parsing::formatConfig(source), sun::support::SunError);
  }
}

/** Configs participate in recursive checks and rewrites alongside source files.
 */
TEST_F(Tooling_FmtIgnore, FormatsConfigs) {
  const auto path = (dir / "sun-config.json").string();
  std::ofstream(path) << R"({"steps":[],"root":true,"entrypoints":[]})";
  write("main.sun", "function f()void{}");
  EXPECT_EQ(check("."), 1);
  EXPECT_EQ(read("sun-config.json"),
            R"({"steps":[],"root":true,"entrypoints":[]})");
  EXPECT_EQ(sun::cli::runFmtCommand({dir.string()}), 0);
  EXPECT_EQ(check("."), 0);
  EXPECT_EQ(read("main.sun"), "function f() void {}\n");
  std::ofstream(path) << "{broken";
  EXPECT_EQ(sun::cli::runFmtCommand({"--no-ignore", path}), 2);
  EXPECT_EQ(read("sun-config.json"), "{broken");
}

/** Config file exclusions can be bypassed explicitly. */
TEST_F(Tooling_FmtIgnore, IgnoresConfigFile) {
  const auto path = (dir / "sun-config.json").string();
  std::ofstream(path)
      << R"({"root":true,"sun_fmt":{"ignore":["sun-config.json"]}})";
  EXPECT_EQ(check("sun-config.json"), 0);
  EXPECT_EQ(check("sun-config.json", true), 1);
}

/** Block footer comments remain separate from trailing field comments. */
TEST(Tooling_Fmt, ManifestFooterComments) {
  const auto result = fmt(R"(manifest {
  libraries: [] // Libraries.
  // Manifest footer.
})");
  EXPECT_EQ(result,
            "manifest {\n  libraries: [] // Libraries.\n"
            "  // Manifest footer.\n}\n");
  EXPECT_EQ(fmt(result), result);
  EXPECT_EQ(fmt("manifest {\n// Empty manifest.\n}"),
            "manifest {\n  // Empty manifest.\n}\n");
}

/** Every supported root field has a fixed position. */
TEST(Tooling_FmtConfig, CompleteRootOrder) {
  const std::vector<std::string> keys = {
      "package_id", "root",           "supported_targets",
      "sun_path",   "path_variables", "exception_trace_depth",
      "sun_fmt",    "projects",       "entrypoints",
      "packages",   "exports",        "dependencies",
      "c_bindings", "target",         "steps"};
  std::string input = "{";
  for (auto key = keys.rbegin(); key != keys.rend(); ++key) {
    if (key != keys.rbegin()) {
      input += ",";
    }
    input += "\"" + *key + "\":null";
  }
  const auto result = sun::parsing::formatConfig(input + "}");
  size_t previous = 0;
  for (const auto& key : keys) {
    const auto position = result.find("\"" + key + "\"");
    EXPECT_GT(position, previous) << key;
    previous = position;
  }
}

/** Default-only objects and path-variable alternatives use their value schemas.
 */
TEST(Tooling_FmtConfig, DefaultAndPathVariableSchemas) {
  const auto result = sun::parsing::formatConfig(R"({
    "path_variables":{"Z":{"target":{"linux":"linux"},"default":"base"},
                      "A":"a"},
    "entrypoints":{"default":[{"type":"binary","path":"main.sun","name":"app"}]},
    "custom":{"target":{"z":1},"default":2}
  })");
  EXPECT_LT(result.find("\"Z\""), result.find("\"A\""));
  EXPECT_LT(result.find("\"default\""), result.find("\"target\""));
  EXPECT_LT(result.find("\"name\""), result.find("\"path\""));
  const auto custom = result.substr(result.find("\"custom\""));
  EXPECT_LT(custom.find("\"target\""), custom.find("\"default\""));
  EXPECT_EQ(sun::parsing::formatConfig(result), result);
}

/** Comments between manifest syntax tokens survive field ordering. */
TEST(Tooling_Fmt, ManifestInterleavedComments) {
  const auto result = fmt(R"(manifest /* Dependencies. */ {
  libraries /* Libraries. */ : /* Paths. */ ["std.moon"]
  source_files: /* Sources. */ ["main.sun"]
})");
  EXPECT_LT(result.find("source_files:"), result.find("libraries"));
  for (const auto* comment : {"/* Dependencies. */", "/* Libraries. */",
                              "/* Paths. */", "/* Sources. */"}) {
    EXPECT_NE(result.find(comment), std::string::npos);
  }
  EXPECT_EQ(fmt(result), result);
}

/** Environment variable names are not target-selection field names. */
TEST(Tooling_FmtConfig, PreservesEnvironmentMapKeys) {
  const auto result = sun::parsing::formatConfig(R"({"steps":[{
    "name":"build","environment":{"target":"linux","default":"release"}
  }]})");
  EXPECT_LT(result.find("\"target\""), result.find("\"default\""));
  EXPECT_EQ(sun::parsing::formatConfig(result), result);
}

/** Comments before optional section separators do not prevent field ordering.
 */
TEST(Tooling_Fmt, ManifestCommentBeforeSeparator) {
  const auto result = fmt(R"(manifest {
  libraries: [] /* Libraries. */ ,
  source_files: []
})");
  EXPECT_LT(result.find("source_files:"), result.find("libraries:"));
  EXPECT_NE(result.find("libraries: [] /* Libraries. */ ,"), std::string::npos);
  EXPECT_EQ(fmt(result), result);
}
