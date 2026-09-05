// tests/tooling/lsp/test_definition.cpp — Go-to-definition
//
// Each test analyzes a small program without generating code, asks for the
// definition of the symbol at the first byte of a distinctive snippet, and
// checks that the answer is the declared name's range.

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
#include "lsp/definition.h"

namespace {

// The file never exists on disk; nodes carry the path exactly as given
const char* kPath = "/definition_test.sun";

struct Analysis {
  std::unique_ptr<Driver> driver;
  Driver::AnalyzedProgram program;
};

Analysis analyze(const std::string& source, bool withStdlib = false) {
  initTestEnvironment();
  Analysis analysis;
  analysis.driver = Driver::createForAOT("definition_test");
  if (withStdlib) analysis.driver->setMoonImports(getStdlibMoonImports());
  analysis.program = analysis.driver->analyzeString(source, kPath);
  return analysis;
}

// Byte offset of the Nth occurrence of needle
size_t offsetOf(const std::string& source, const std::string& needle,
                int occurrence = 0) {
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

std::optional<sun::lsp::SymbolLocation> definitionAt(const std::string& source,
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
  return sun::lsp::computeDefinition(*analysis.program.ast, kPath, source,
                                     static_cast<int>(pos));
}

std::string rangeText(const std::string& text,
                      const sun::lsp::SymbolLocation& def) {
  return text.substr(
      def.range.offset,
      def.range.endOffset.value_or(def.range.offset) - def.range.offset);
}

// The definition of the symbol at `needle` is the name starting at
// `declaration` in the same document
testing::AssertionResult definedAt(const std::string& source,
                                   const std::string& needle,
                                   const std::string& declaration,
                                   int declarationOccurrence = 0,
                                   int needleOccurrence = 0) {
  auto def = definitionAt(source, needle, false, needleOccurrence);
  if (!def) {
    return testing::AssertionFailure() << "no definition for " << needle;
  }
  if (def->filePath != kPath) {
    return testing::AssertionFailure()
           << "definition of " << needle << " is in " << def->filePath;
  }
  size_t expected = offsetOf(source, declaration, declarationOccurrence);
  if (static_cast<size_t>(def->range.offset) != expected) {
    return testing::AssertionFailure()
           << "definition of " << needle << " is at offset "
           << def->range.offset << " (" << rangeText(source, *def)
           << "), expected " << expected << " (" << declaration << ")";
  }
  std::string name = rangeText(source, *def);
  if (name.empty() || declaration.compare(0, name.size(), name) != 0) {
    return testing::AssertionFailure()
           << "definition of " << needle << " covers '" << name << "'";
  }
  return testing::AssertionSuccess();
}

std::string readFile(const std::string& path) {
  std::ifstream file(path);
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

const char* kProgram = R"(
class Point {
    var x: i32;
    var y: i32;
    init(x: i32, y: i32) { this.x = x; this.y = y; }
    const method sum() i32 { return this.x + this.y; }
}

interface IShape {
    method area() i32;
}

class Square implements IShape {
    var side: i32;
    init(side: i32) { this.side = side; }
    method area() i32 { return this.side * this.side; }
}

enum Shape { Circle(i32), Empty }

function add(a: i32, b: i32) i32 {
    var total: i32 = a + b;
    const k = 42;
    return total + k;
}

function measure(s: Shape) i32 {
    var result = match s {
        Shape.Circle(radius) => radius * 3,
        _ => 0
    };
    return result;
}

function describe(shape: ref IShape) i32 { return shape.area(); }

function main() i32 {
    var p = Point(1, 2);
    ref r = p;
    var s = r.sum();
    var q = p.x;
    var sq = Square(3);
    var shape = Shape.Circle(2);
    var m = measure(shape);
    return add(s, q) + describe(sq) + m;
}
)";

}  // namespace

TEST(Tooling_Lsp_Definition, LocalVariable) {
  EXPECT_TRUE(definedAt(kProgram, "total + k", "total: i32"));
}

TEST(Tooling_Lsp_Definition, ConstLocal) {
  EXPECT_TRUE(definedAt(kProgram, "k;", "k = 42"));
}

TEST(Tooling_Lsp_Definition, Parameter) {
  EXPECT_TRUE(definedAt(kProgram, "a + b", "a: i32, b"));
  EXPECT_TRUE(definedAt(kProgram, "b;", "b: i32) i32"));
}

TEST(Tooling_Lsp_Definition, MethodParameterShadowingField) {
  // `this.x = x`: the right-hand `x` is the parameter, not the field
  EXPECT_TRUE(definedAt(kProgram, "x; this.y", "x: i32, y: i32"));
}

TEST(Tooling_Lsp_Definition, ReferenceBinding) {
  EXPECT_TRUE(definedAt(kProgram, "r.sum()", "r = p"));
}

TEST(Tooling_Lsp_Definition, MatchBinding) {
  EXPECT_TRUE(definedAt(kProgram, "radius * 3", "radius)"));
}

TEST(Tooling_Lsp_Definition, FunctionCall) {
  EXPECT_TRUE(definedAt(kProgram, "add(s, q)", "add(a: i32"));
  EXPECT_TRUE(definedAt(kProgram, "measure(shape)", "measure(s: Shape)"));
}

TEST(Tooling_Lsp_Definition, MethodCall) {
  EXPECT_TRUE(definedAt(kProgram, "sum();", "sum() i32"));
}

TEST(Tooling_Lsp_Definition, FieldAccess) {
  EXPECT_TRUE(definedAt(kProgram, "x;\n", "x: i32;"));
}

TEST(Tooling_Lsp_Definition, This) {
  EXPECT_TRUE(definedAt(kProgram, "this.x = x", "Point {"));
}

TEST(Tooling_Lsp_Definition, ConstructorCall) {
  EXPECT_TRUE(definedAt(kProgram, "Point(1, 2)", "Point {"));
  EXPECT_TRUE(definedAt(kProgram, "Square(3)", "Square implements"));
}

TEST(Tooling_Lsp_Definition, EnumAndVariant) {
  EXPECT_TRUE(definedAt(kProgram, "Shape.Circle(2)", "Shape { Circle"));
  EXPECT_TRUE(definedAt(kProgram, "Circle(2)", "Circle(i32)"));
}

TEST(Tooling_Lsp_Definition, TypeNamesInAnnotations) {
  EXPECT_TRUE(definedAt(kProgram, "Shape) i32", "Shape { Circle"));
  EXPECT_TRUE(definedAt(kProgram, "IShape) i32", "IShape {"));
}

TEST(Tooling_Lsp_Definition, InterfaceMethod) {
  EXPECT_TRUE(definedAt(kProgram, "area(); }", "area() i32;"));
}

TEST(Tooling_Lsp_Definition, DefinitionHeaderIsItself) {
  EXPECT_TRUE(definedAt(kProgram, "class Point", "Point {"));
  EXPECT_TRUE(definedAt(kProgram, "Point {", "Point {"));
  EXPECT_TRUE(definedAt(kProgram, "function add", "add(a: i32"));
  EXPECT_TRUE(definedAt(kProgram, "var total", "total: i32"));
  EXPECT_TRUE(definedAt(kProgram, "const k", "k = 42"));
  EXPECT_TRUE(definedAt(kProgram, "enum Shape", "Shape { Circle"));
  EXPECT_TRUE(definedAt(kProgram, "Circle(i32)", "Circle(i32)"));
  EXPECT_TRUE(definedAt(kProgram, "y: i32;", "y: i32;"));
  EXPECT_TRUE(definedAt(kProgram, "radius)", "radius)"));
}

TEST(Tooling_Lsp_Definition, NothingForLiteralsStatementsAndWhitespace) {
  EXPECT_FALSE(definitionAt(kProgram, "42;"));
  EXPECT_FALSE(definitionAt(kProgram, "return total"));
  EXPECT_FALSE(definitionAt(kProgram, "\n\nfunction add"));
  EXPECT_FALSE(definitionAt(kProgram, "* 3"));
}

TEST(Tooling_Lsp_Definition, GenericFunctionBody) {
  std::string source = R"(
class Pool {
    var n: i32;
    init() { this.n = 0; }
}
function take<T>(p: ref Pool, v: T) T {
    var copy = v;
    return copy;
}
function main() i32 {
    var pool = Pool();
    return take<i32>(pool, 1);
}
)";
  // Inside the body the chain runs through the specialization clone, which
  // keeps the template's spans
  EXPECT_TRUE(definedAt(source, "v;", "v: T"));
  EXPECT_TRUE(definedAt(source, "copy;", "copy = v"));
  EXPECT_TRUE(definedAt(source, "Pool, v", "Pool {"));
  EXPECT_TRUE(definedAt(source, "take<i32>", "take<T>(p"));
}

TEST(Tooling_Lsp_Definition, GenericClassBody) {
  std::string source = R"(
class Box<T> {
    var value: T;
    init(v: T) { this.value = v; }
    method get() T { return this.value; }
}
function main() i32 {
    var b = Box<i32>(7);
    return b.get();
}
)";
  EXPECT_TRUE(definedAt(source, "v; }", "v: T"));
  EXPECT_TRUE(definedAt(source, "value; }", "value: T;"));
  EXPECT_TRUE(definedAt(source, "get();", "get() T"));
  EXPECT_TRUE(definedAt(source, "Box<i32>", "Box<T>"));
}

TEST(Tooling_Lsp_Definition, ModuleQualifiedAccess) {
  std::string source = R"(
public module util {
    public function twice(v: i32) i32 { return v * 2; }
}
function main() i32 {
    return util.twice(4);
}
)";
  EXPECT_TRUE(definedAt(source, "twice(4)", "twice(v: i32)"));
}

TEST(Tooling_Lsp_Definition, Lambda) {
  std::string source = R"(
function main() i32 {
    var base = 10;
    var f = (delta: i32) => i32 { return base + delta; };
    return f(1);
}
)";
  EXPECT_TRUE(definedAt(source, "delta; }", "delta: i32"));
  EXPECT_TRUE(definedAt(source, "base + delta", "base = 10"));
  EXPECT_TRUE(definedAt(source, "f(1)", "f = (delta: i32)"));
}

TEST(Tooling_Lsp_Definition, CatchBinding) {
  std::string source = R"(
class Oops implements IError {
    init() {}
    method code() i32 { return 1; }
    method message() static_ptr<u8> { return "oops"; }
}
function risky(x: i32) i32 throws IError {
    if (x < 0) { throw Oops(); }
    return x;
}
function main() i32 {
    try {
        return risky(1);
    } catch (err: IError) {
        return err.code();
    }
}
)";
  EXPECT_TRUE(definedAt(source, "err.code()", "err: IError"));
  EXPECT_TRUE(definedAt(source, "Oops();", "Oops implements"));
  EXPECT_TRUE(definedAt(source, "risky(1)", "risky(x: i32)"));
}

TEST(Tooling_Lsp_Definition, MergedFiles) {
  initTestEnvironment();
  std::filesystem::create_directories("tmp");
  std::string mainPath =
      std::filesystem::absolute("tmp/definition_main.sun").string();
  std::string helperPath =
      std::filesystem::absolute("tmp/definition_helper.sun").string();
  std::string helperText = "function helper() i32 {\n    return 1;\n}\n";
  std::string mainText = "function main() i32 {\n    return helper();\n}\n";
  {
    std::ofstream file(helperPath);
    file << helperText;
  }
  {
    std::ofstream file(mainPath);
    file << mainText;
  }
  std::string canonicalHelper = std::filesystem::canonical(helperPath).string();

  auto driver = Driver::createForAOT("definition_test");
  auto program = driver->analyzeFiles({mainPath, helperPath}, {}, {}, {});
  ASSERT_TRUE(program.ast);
  EXPECT_FALSE(program.error.has_value());

  auto def = sun::lsp::computeDefinition(
      *program.ast, mainPath, mainText,
      static_cast<int>(offsetOf(mainText, "helper()")));
  ASSERT_TRUE(def);
  EXPECT_EQ(def->filePath, canonicalHelper);
  EXPECT_EQ(def->range.offset,
            static_cast<int>(offsetOf(helperText, "helper() i32")));
  EXPECT_EQ(rangeText(helperText, *def), "helper");
  EXPECT_EQ(def->start.line, 0);
  EXPECT_EQ(def->start.character, 9);
  EXPECT_EQ(def->end.character, 15);
  std::filesystem::remove(mainPath);
  std::filesystem::remove(helperPath);
}

TEST(Tooling_Lsp_Definition, StdlibDeclarations) {
  if (getStdlibMoonImports().empty()) GTEST_SKIP() << "stdlib.moon not built";
  std::string source = R"(
using std;
class Config {
    var n: i64;
    init(alloc: ref HeapAllocator) { this.n = 1; }
}
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
  auto expectStdlib = [&](const std::string& needle, const std::string& file,
                          const std::string& name) {
    auto def = definitionAt(source, needle, true);
    ASSERT_TRUE(def) << "no definition for " << needle;
    EXPECT_TRUE(def->filePath.size() > file.size() &&
                def->filePath.compare(def->filePath.size() - file.size(),
                                      file.size(), file) == 0)
        << def->filePath;
    EXPECT_EQ(rangeText(readFile(def->filePath), *def), name)
        << "at offset " << def->range.offset << " line " << def->range.line;
  };
  expectStdlib("push(1)", "stdlib/vec.sun", "push");
  expectStdlib("Vec<i64>(", "stdlib/vec.sun", "Vec");
  expectStdlib("make_heap_allocator()", "stdlib/allocator.sun",
               "make_heap_allocator");
  expectStdlib("HeapAllocator)", "stdlib/allocator.sun", "HeapAllocator");
  // The loop variable is declared in the document itself
  auto item = definitionAt(source, "item;", true);
  ASSERT_TRUE(item);
  EXPECT_EQ(item->filePath, kPath);
  EXPECT_EQ(item->range.offset,
            static_cast<int>(offsetOf(source, "item: i64 in v")));
  EXPECT_EQ(rangeText(source, *item), "item");
}

TEST(Tooling_Lsp_Definition, FieldInitializerIgnoresConstructorParameter) {
  const std::string source = R"(
    public module settings { public var seed: i32 = 10; }
    using settings;
    class Foo {
      var x: i32 = seed + 2;
      init(seed: bool) {}
    }
  )";
  EXPECT_TRUE(definedAt(source, "seed +", "seed: i32"));
}
