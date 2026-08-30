// tests/tooling/lsp/test_hover.cpp — Hover type information
//
// Each test analyzes a small program without generating code, hovers at the
// first byte of a distinctive snippet, and checks the Sun-syntax text the
// language server would show.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include "ast.h"
#include "driver/driver.h"
#include "driver/execution_utils.h"
#include "lsp/hover.h"
#include "parsing/doc_comments.h"
#include "parsing/parser.h"

namespace {

// The file never exists on disk; nodes carry the path exactly as given
const char* kPath = "/hover_test.sun";

struct Analysis {
  std::unique_ptr<Driver> driver;
  Driver::AnalyzedProgram program;
};

Analysis analyze(const std::string& source, bool withStdlib = false) {
  initTestEnvironment();
  Analysis analysis;
  analysis.driver = Driver::createForAOT("hover_test");
  if (withStdlib) analysis.driver->setMoonImports(getStdlibMoonImports());
  analysis.program = analysis.driver->analyzeString(source, kPath);
  return analysis;
}

// Byte offset of the Nth occurrence of needle
size_t offsetOf(const std::string& source, const std::string& needle,
                int occurrence) {
  size_t pos = std::string::npos;
  size_t from = 0;
  for (int i = 0; i <= occurrence; ++i) {
    pos = source.find(needle, from);
    if (pos == std::string::npos) break;
    from = pos + 1;
  }
  EXPECT_NE(pos, std::string::npos) << "needle not found: " << needle;
  return pos;
}

std::optional<sun::lsp::Hover> fullHoverAt(const std::string& source,
                                           const std::string& needle,
                                           bool withStdlib = false,
                                           int occurrence = 0) {
  size_t pos = offsetOf(source, needle, occurrence);
  if (pos == std::string::npos) return std::nullopt;
  Analysis analysis = analyze(source, withStdlib);
  if (!analysis.program.ast) {
    ADD_FAILURE() << "program did not parse: "
                  << (analysis.program.error ? analysis.program.error->what()
                                             : "");
    return std::nullopt;
  }
  return sun::lsp::computeHover(*analysis.program.ast, kPath, source,
                                static_cast<int>(pos));
}

std::optional<std::string> hoverAt(const std::string& source,
                                   const std::string& needle,
                                   bool withStdlib = false,
                                   int occurrence = 0) {
  auto hover = fullHoverAt(source, needle, withStdlib, occurrence);
  if (!hover) return std::nullopt;
  return hover->code;
}

// The documentation shown for the symbol at needle
std::optional<std::string> docAt(const std::string& source,
                                 const std::string& needle,
                                 int occurrence = 0) {
  auto hover = fullHoverAt(source, needle, false, occurrence);
  if (!hover) return std::nullopt;
  return hover->documentation;
}

const char* kProgram = R"(
class Point {
    var x: i32;
    var y: i32;
    init(x: i32, y: i32) { this.x = x; this.y = y; }
    const method sum() i32 { return this.x + this.y; }
}

function add(a: i32, b: i32) i32 {
    var total: i32 = a + b;
    const k = 42;
    var n = 1;
    return total + k + n;
}

function read(p: const ref Point) i32 { return p.x; }

function mayFail(v: i32) i32 throws IError { return v; }

function main() i32 {
    var p = Point(1, 2);
    var s = p.sum();
    var q = p.x;
    var flag = true;
    if (flag) { return read(p) + q + s + add(1, 2) + mayFail(3); }
    return 0;
}
)";

}  // namespace

TEST(Tooling_Lsp_Hover, VariableReference) {
  EXPECT_EQ(hoverAt(kProgram, "total + k"), "total: i32");
}

TEST(Tooling_Lsp_Hover, TypedDeclaration) {
  EXPECT_EQ(hoverAt(kProgram, "var total"), "var total: i32");
}

TEST(Tooling_Lsp_Hover, InferredDeclaration) {
  EXPECT_EQ(hoverAt(kProgram, "var n = 1"), "var n: i32");
}

TEST(Tooling_Lsp_Hover, ConstDeclaration) {
  EXPECT_EQ(hoverAt(kProgram, "const k"), "const k: i32");
}

TEST(Tooling_Lsp_Hover, ClassInstance) {
  EXPECT_EQ(hoverAt(kProgram, "p.sum()"), "p: Point");
}

TEST(Tooling_Lsp_Hover, FieldAccess) {
  EXPECT_EQ(hoverAt(kProgram, "x;", false, 0), "x: i32");
}

TEST(Tooling_Lsp_Hover, MethodCallResult) {
  EXPECT_EQ(hoverAt(kProgram, "var s = p.sum()"), "var s: i32");
}

TEST(Tooling_Lsp_Hover, MethodCallee) {
  EXPECT_EQ(hoverAt(kProgram, "sum();"), "sum: () i32");
}

TEST(Tooling_Lsp_Hover, FunctionSignatureOnKeyword) {
  EXPECT_EQ(hoverAt(kProgram, "function add"),
            "function add(a: i32, b: i32) i32");
}

TEST(Tooling_Lsp_Hover, FunctionSignatureOnName) {
  std::string source = kProgram;
  size_t pos = offsetOf(source, "function add", 0) + 9;  // on "add"
  Analysis analysis = analyze(source);
  ASSERT_TRUE(analysis.program.ast);
  auto hover = sun::lsp::computeHover(*analysis.program.ast, kPath, source,
                                      static_cast<int>(pos));
  ASSERT_TRUE(hover);
  EXPECT_EQ(hover->code, "function add(a: i32, b: i32) i32");
  // The range covers the whole function
  EXPECT_EQ(hover->range.offset, static_cast<int>(pos - 9));
}

TEST(Tooling_Lsp_Hover, ThrowingFunction) {
  EXPECT_EQ(hoverAt(kProgram, "function mayFail"),
            "function mayFail(v: i32) i32 throws IError");
}

TEST(Tooling_Lsp_Hover, ConstMethod) {
  EXPECT_EQ(hoverAt(kProgram, "const method sum"), "const method sum() i32");
}

TEST(Tooling_Lsp_Hover, ConstRefParameter) {
  EXPECT_EQ(hoverAt(kProgram, "p.x; }"), "p: const ref Point");
}

TEST(Tooling_Lsp_Hover, FunctionCallee) {
  EXPECT_EQ(hoverAt(kProgram, "add(1, 2)"), "add: (i32, i32) i32");
}

TEST(Tooling_Lsp_Hover, ClassHeaderAndField) {
  EXPECT_EQ(hoverAt(kProgram, "class Point"), "class Point");
  EXPECT_EQ(hoverAt(kProgram, "y: i32;"), "var y: i32");
}

TEST(Tooling_Lsp_Hover, This) {
  EXPECT_EQ(hoverAt(kProgram, "this.x + this.y"), "this: Point");
}

TEST(Tooling_Lsp_Hover, Literals) {
  EXPECT_EQ(hoverAt(kProgram, "42;"), "i32");
  EXPECT_EQ(hoverAt(kProgram, "true;"), "bool");
}

TEST(Tooling_Lsp_Hover, BinaryExpressionOnOperator) {
  EXPECT_EQ(hoverAt(kProgram, "+ b;"), "i32");
}

TEST(Tooling_Lsp_Hover, StatementsAndWhitespaceShowNothing) {
  EXPECT_FALSE(hoverAt(kProgram, "if (flag)"));
  EXPECT_FALSE(hoverAt(kProgram, "return 0;"));
  EXPECT_FALSE(hoverAt(kProgram, "\n\nfunction add"));
}

TEST(Tooling_Lsp_Hover, GenericClassInstance) {
  std::string source = R"(
class Box<T> {
    var value: T;
    init(v: T) { this.value = v; }
}
function main() i32 {
    var b = Box<i32>(7);
    return 0;
}
)";
  EXPECT_EQ(hoverAt(source, "var b"), "var b: Box<i32>");
  EXPECT_EQ(hoverAt(source, "class Box"), "class Box<T>");
}

TEST(Tooling_Lsp_Hover, GenericClassBody) {
  std::string source = R"(
class Box<T> {
    var value: T;
    init(v: T) { this.value = v; }
    method get() T { return this.value; }
    method peek() ref T { return this.value; }
}
function main() i32 {
    var b = Box<i32>(7);
    return b.get();
}
)";
  // Inside the body, types come from the Box<i32> specialization but are
  // shown in terms of T
  EXPECT_EQ(hoverAt(source, "value; }"), "value: T");
  EXPECT_EQ(hoverAt(source, "value; }", false, 1), "value: T");
  EXPECT_EQ(hoverAt(source, "this.value = v"), "this: Box<T>");
  EXPECT_EQ(hoverAt(source, "v; }"), "v: T");
  EXPECT_EQ(hoverAt(source, "method get"), "method get() T");
  EXPECT_EQ(hoverAt(source, "method peek"), "method peek() ref T");
  EXPECT_EQ(hoverAt(source, "value: T;"), "var value: T");
  EXPECT_EQ(hoverAt(source, "class Box"), "class Box<T>");
}

TEST(Tooling_Lsp_Hover, GenericFunctionBody) {
  std::string source = R"(
function identity<T>(x: T) T {
    var copy = x;
    return copy;
}
function main() i32 {
    return identity<i32>(1);
}
)";
  EXPECT_EQ(hoverAt(source, "var copy"), "var copy: T");
  EXPECT_EQ(hoverAt(source, "copy;"), "copy: T");
  EXPECT_EQ(hoverAt(source, "function identity"),
            "function identity<T>(x: T) T");
}

TEST(Tooling_Lsp_Hover, UnusedGenericFunction) {
  // A generic function body is analyzed once with T as a type parameter, so
  // hover works even when nothing instantiates it
  std::string source = R"(
function lone<T>(x: T) T {
    var copy: T = x;
    return copy;
}
)";
  EXPECT_EQ(hoverAt(source, "function lone"), "function lone<T>(x: T) T");
  EXPECT_EQ(hoverAt(source, "var copy"), "var copy: T");
  EXPECT_EQ(hoverAt(source, "copy;"), "copy: T");
  EXPECT_EQ(hoverAt(source, "x;"), "x: T");
}

TEST(Tooling_Lsp_Hover, UnusedGenericClassShowsAnnotationsOnly) {
  // A generic class body is only analyzed through its specializations
  std::string source = R"(
class Lone<T> {
    var value: T;
    init(v: T) { this.value = v; }
    method get() T { return this.value; }
}
)";
  EXPECT_EQ(hoverAt(source, "class Lone"), "class Lone<T>");
  EXPECT_EQ(hoverAt(source, "value: T;"), "var value: T");
  EXPECT_EQ(hoverAt(source, "method get"), "method get() T");
  EXPECT_FALSE(hoverAt(source, "value; }"));
}

TEST(Tooling_Lsp_Hover, DocComments) {
  std::string source = R"(
// Adds two numbers.
// Returns the sum.
function add(a: i32, b: i32) i32 { return a + b; }

/* A point on the plane. */
class Point {
    // Horizontal position.
    var x: i32;
    init() { this.x = 0; }
    /// Distance from the origin.
    method len() i32 { return this.x; }
}

// Not attached: a blank line follows.

function main() i32 {
    // The running total.
    var total = add(1, 2);
    var p = Point();
    return total + p.len() + p.x;
}
)";
  EXPECT_EQ(docAt(source, "function add"),
            "Adds two numbers.\nReturns the sum.");
  EXPECT_EQ(docAt(source, "add(1, 2)"), "Adds two numbers.\nReturns the sum.");
  EXPECT_EQ(docAt(source, "class Point"), "A point on the plane.");
  EXPECT_EQ(docAt(source, "Point();"), "A point on the plane.");
  EXPECT_EQ(docAt(source, "x: i32;"), "Horizontal position.");
  EXPECT_EQ(docAt(source, "method len"), "Distance from the origin.");
  EXPECT_EQ(docAt(source, "len() +"), "Distance from the origin.");
  EXPECT_EQ(docAt(source, "x;\n}"), "Horizontal position.");
  EXPECT_EQ(docAt(source, "var total"), "The running total.");
  EXPECT_EQ(docAt(source, "total + p"), "The running total.");
  EXPECT_EQ(docAt(source, "function main"), "");
  EXPECT_EQ(docAt(source, "this.x = 0"), "A point on the plane.");
}

TEST(Tooling_Lsp_Hover, TypeNamesInAnnotations) {
  std::string source = R"(
/**
 * A pool of objects.
 * Hands them out one at a time.
 */
class Pool { init() {} }
// Which end of the pool to take from.
enum End { Front, Back }
function use(p: ref Pool, e: End) i32 {
    var q: Pool = Pool();
    return 0;
}
)";
  const char* poolDoc = "A pool of objects.\nHands them out one at a time.";
  // Parameter, local annotation, and constructor call all name the class
  auto param = fullHoverAt(source, "Pool, e");
  ASSERT_TRUE(param);
  EXPECT_EQ(param->code, "class Pool");
  EXPECT_EQ(param->documentation, poolDoc);
  // The range is the type name itself, not the `ref` wrapper around it
  EXPECT_EQ(param->range.offset,
            static_cast<int>(offsetOf(source, "Pool, e", 0)));
  EXPECT_EQ(hoverAt(source, "Pool = Pool()"), "class Pool");
  EXPECT_EQ(docAt(source, "Pool = Pool()"), poolDoc);
  EXPECT_EQ(docAt(source, "Pool();"), poolDoc);
  EXPECT_EQ(hoverAt(source, "End) i32"), "enum End");
  EXPECT_EQ(docAt(source, "End) i32"), "Which end of the pool to take from.");
  // Primitive names keep the enclosing hover
  EXPECT_EQ(hoverAt(source, "i32 {"), "function use(p: ref Pool, e: End) i32");
}

TEST(Tooling_Lsp_Hover, AttachedDocsSurviveCloning) {
  // attachDocComments stores comments on declarations; clone() goes through
  // the serializer, so this is the same path a .moon bundle takes
  std::string source = R"(
// Counts things.
class Counter {
    // How many so far.
    var count: i32;
    init() { this.count = 0; }
    // One more.
    method bump() void { this.count = this.count + 1; }
}
// Which way to go.
enum Direction {
    // Skyward.
    Up,
    Down
}
/// Ready to use.
function make() Counter { return Counter(); }
// Flat.
enum Level { Low, High }
)";
  std::istringstream stream(source);
  Parser parser(stream);
  auto program = parser.parseString(source);
  ASSERT_TRUE(program);
  sun::attachDocComments(*program, source);

  auto cls = program->getBody()[0]->clone();
  const auto& counter = static_cast<const ClassDefinitionAST&>(*cls);
  EXPECT_EQ(counter.getDoc(), "Counts things.");
  EXPECT_EQ(counter.getFields()[0].doc, "How many so far.");
  EXPECT_EQ(counter.getMethods()[1].function->getProto().getDoc(), "One more.");

  auto enumNode = program->getBody()[1]->clone();
  const auto& direction = static_cast<const EnumDefinitionAST&>(*enumNode);
  EXPECT_EQ(direction.getDoc(), "Which way to go.");
  EXPECT_EQ(direction.getVariants()[0].doc, "Skyward.");
  EXPECT_EQ(direction.getVariants()[1].doc, "");

  auto fn = program->getBody()[2]->clone();
  EXPECT_EQ(static_cast<const FunctionAST&>(*fn).getProto().getDoc(),
            "Ready to use.");

  // Variants on the enum's own line take nothing from the enum's comment
  auto level = program->getBody()[3]->clone();
  const auto& levelEnum = static_cast<const EnumDefinitionAST&>(*level);
  EXPECT_EQ(levelEnum.getDoc(), "Flat.");
  EXPECT_EQ(levelEnum.getVariants()[0].doc, "");
}

TEST(Tooling_Lsp_Hover, StdlibDocComments) {
  if (getStdlibMoonImports().empty()) GTEST_SKIP() << "stdlib.moon not built";
  // Declarations from the stdlib bundle are documented from the source
  // files the bundle was built from
  std::string source = R"(
using std;
function take(a: ref HeapAllocator) i32 { return 0; }
function main() i32 {
    var allocator = make_heap_allocator();
    return take(allocator);
}
)";
  auto annotation = fullHoverAt(source, "HeapAllocator) i32", true);
  ASSERT_TRUE(annotation);
  EXPECT_EQ(annotation->code,
            "public class HeapAllocator implements IAllocator");
  EXPECT_NE(annotation->documentation.find("default system allocator"),
            std::string::npos)
      << annotation->documentation;
  auto call = fullHoverAt(source, "make_heap_allocator()", true);
  ASSERT_TRUE(call);
  EXPECT_FALSE(call->documentation.empty());
}

TEST(Tooling_Lsp_Hover, ReferenceBinding) {
  std::string source = R"(
class Point { var x: i32; init(v: i32) { this.x = v; } }
function main() i32 {
    var p = Point(1);
    ref r = p;
    const ref c = p;
    return r.x + c.x;
}
)";
  EXPECT_EQ(hoverAt(source, "ref r = p"), "ref r: Point");
  EXPECT_EQ(hoverAt(source, "const ref c"), "const ref c: Point");
  EXPECT_EQ(hoverAt(source, "r.x"), "r: ref Point");
}

TEST(Tooling_Lsp_Hover, EnumAndMatchBinding) {
  std::string source = R"(
enum Shape { Circle(f64), Square }
function area(s: Shape) f64 {
    var result = match s {
        Shape.Circle(radius) => radius * 3.0,
        _ => 0.0
    };
    return result;
}
)";
  EXPECT_EQ(hoverAt(source, "enum Shape"), "enum Shape");
  EXPECT_EQ(hoverAt(source, "Circle(f64)"), "Shape.Circle(f64)");
  EXPECT_EQ(hoverAt(source, "radius)"), "radius: f64");
  EXPECT_EQ(hoverAt(source, "radius * 3.0"), "radius: f64");
}

TEST(Tooling_Lsp_Hover, PartialAnalysisKeepsEarlierTypes) {
  std::string source = R"(
function first() i32 {
    var value: i32 = 1;
    return value;
}
function broken() i32 {
    var oops: i32 = true;
    return oops;
}
)";
  Analysis analysis = analyze(source);
  ASSERT_TRUE(analysis.program.ast);
  ASSERT_TRUE(analysis.program.error.has_value());
  auto hover = sun::lsp::computeHover(
      *analysis.program.ast, kPath, source,
      static_cast<int>(offsetOf(source, "return value", 0) + 7));
  ASSERT_TRUE(hover);
  EXPECT_EQ(hover->code, "value: i32");
}

TEST(Tooling_Lsp_Hover, InnermostNodeWins) {
  std::string source = kProgram;
  Analysis analysis = analyze(source);
  ASSERT_TRUE(analysis.program.ast);
  // Inside `read(p)`, the argument is the innermost node, not the call
  const ExprAST* node = sun::lsp::findInnermostNodeAt(
      *analysis.program.ast, kPath,
      static_cast<int>(offsetOf(source, "read(p)", 0) + 5));
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->getType(), ASTNodeType::VARIABLE_REFERENCE);
}

TEST(Tooling_Lsp_Hover, MergedFilesUseInMemoryOverride) {
  initTestEnvironment();
  // The file on disk is stale; the editor buffer passed as an override wins
  std::filesystem::create_directories("tmp");
  std::string path = std::filesystem::absolute("tmp/hover_override.sun");
  {
    std::ofstream file(path);
    file << "function stale() i32 { return 0; }\n";
  }
  std::string buffer =
      "function fresh() i32 {\n    var count: i64 = 1;\n    return 0;\n}\n";
  std::string canonical = std::filesystem::canonical(path).string();

  auto driver = Driver::createForAOT("hover_test");
  auto program = driver->analyzeFiles({path}, {}, {}, {{canonical, buffer}});
  ASSERT_TRUE(program.ast);
  EXPECT_FALSE(program.error.has_value());

  auto hover =
      sun::lsp::computeHover(*program.ast, path, buffer,
                             static_cast<int>(offsetOf(buffer, "count", 0)));
  ASSERT_TRUE(hover);
  EXPECT_EQ(hover->code, "var count: i64");
  std::filesystem::remove(path);
}

TEST(Tooling_Lsp_Hover, StdlibVecAndLoopVariable) {
  if (getStdlibMoonImports().empty()) GTEST_SKIP() << "stdlib.moon not built";
  std::string source = R"(
using std;
function main() i64 {
    var allocator = make_heap_allocator();
    var v = Vec<i64>(allocator, 8);
    v.push(1);
    var sum: i64 = 0;
    for (var item: i64 in v) {
        sum = sum + item;
    }
    return sum;
}
)";
  auto vec = hoverAt(source, "v.push", true);
  ASSERT_TRUE(vec);
  EXPECT_NE(vec->find("Vec<i64>"), std::string::npos) << *vec;
  // Containers iterate by borrow, so the loop variable is a reference
  EXPECT_EQ(hoverAt(source, "for (var item", true), "var item: ref i64");
  EXPECT_EQ(hoverAt(source, "item;", true), "item: ref i64");
}
