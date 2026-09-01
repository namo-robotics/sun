// tests/tooling/lsp/test_references.cpp — Find references
//
// Each test analyzes a small program without generating code, asks for the
// references of the symbol at the first byte of a distinctive snippet, and
// checks that the answer is exactly the expected set of name ranges.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"
#include "driver/driver.h"
#include "driver/execution_utils.h"
#include "lsp/references.h"

namespace {

// The file never exists on disk; nodes carry the path exactly as given
const char* kPath = "/references_test.sun";

struct Analysis {
  std::unique_ptr<Driver> driver;
  Driver::AnalyzedProgram program;
};

Analysis analyze(const std::string& source, bool withStdlib = false) {
  initTestEnvironment();
  Analysis analysis;
  analysis.driver = Driver::createForAOT("references_test");
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

std::vector<sun::lsp::SymbolLocation> referencesAt(const std::string& source,
                                                   const std::string& needle,
                                                   bool includeDeclaration,
                                                   int occurrence = 0,
                                                   bool withStdlib = false) {
  size_t pos = offsetOf(source, needle, occurrence);
  if (pos == std::string::npos) return {};
  Analysis analysis = analyze(source, withStdlib);
  if (!analysis.program.ast) {
    ADD_FAILURE() << "program did not parse: "
                  << (analysis.program.error ? analysis.program.error->what()
                                             : "");
    return {};
  }
  return sun::lsp::computeReferences(*analysis.program.ast, kPath, source,
                                     static_cast<int>(pos), includeDeclaration);
}

std::string rangeText(const std::string& text,
                      const sun::lsp::SymbolLocation& location) {
  return text.substr(location.range.offset,
                     location.range.endOffset.value_or(location.range.offset) -
                         location.range.offset);
}

// Leading identifier of a snippet
std::string identifierOf(const std::string& snippet) {
  size_t length = 0;
  while (length < snippet.size() &&
         (std::isalnum(static_cast<unsigned char>(snippet[length])) ||
          snippet[length] == '_')) {
    ++length;
  }
  return snippet.substr(0, length);
}

std::string describe(const std::string& source,
                     const std::vector<sun::lsp::SymbolLocation>& results) {
  std::string text;
  for (const auto& result : results) {
    if (!text.empty()) text += ", ";
    text += std::to_string(result.range.offset) + " (" +
            rangeText(source, result) + ")";
    if (result.filePath != kPath) text += " in " + result.filePath;
  }
  return text.empty() ? "nothing" : text;
}

// A name expected among the results: the Nth occurrence of a snippet that
// starts with it
struct ExpectedName {
  std::string needle;
  int occurrence = 0;
};

// The references of the symbol at `needle` are exactly the names at
// `expected`, all in the document
testing::AssertionResult refersTo(const std::string& source,
                                  const std::string& needle,
                                  bool includeDeclaration,
                                  const std::vector<ExpectedName>& expected,
                                  int needleOccurrence = 0) {
  auto results =
      referencesAt(source, needle, includeDeclaration, needleOccurrence);
  std::map<int, std::string> wanted;
  for (const auto& item : expected) {
    size_t at = offsetOf(source, item.needle, item.occurrence);
    if (at == std::string::npos) {
      return testing::AssertionFailure() << "bad expectation " << item.needle;
    }
    wanted[static_cast<int>(at)] = identifierOf(item.needle);
  }
  std::string wantedText;
  for (const auto& [offset, name] : wanted) {
    if (!wantedText.empty()) wantedText += ", ";
    wantedText += std::to_string(offset) + " (" + name + ")";
  }
  std::vector<int> got;
  for (const auto& result : results) {
    if (result.filePath != kPath) {
      return testing::AssertionFailure()
             << "references of " << needle << " are "
             << describe(source, results) << ", expected " << wantedText;
    }
    got.push_back(result.range.offset);
  }
  std::vector<int> want;
  for (const auto& [offset, name] : wanted) want.push_back(offset);
  if (got != want) {
    return testing::AssertionFailure()
           << "references of " << needle << " are " << describe(source, results)
           << ", expected " << wantedText;
  }
  for (const auto& result : results) {
    if (rangeText(source, result) != wanted[result.range.offset]) {
      return testing::AssertionFailure()
             << "reference of " << needle << " at " << result.range.offset
             << " covers '" << rangeText(source, result) << "'";
    }
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

class Pair {
    var x: i32;
    var y: i32;
}

enum Shape { Circle(i32), Empty }

function add(a: i32, b: i32) i32 {
    var total: i32 = a + b;
    total = total + 1;
    total += a;
    return total;
}

function measure(s: Shape) i32 {
    var result = match s {
        Shape.Circle(radius) => radius * 3,
        _ => 0
    };
    return result;
}

function describe(shape: ref IShape) i32 { return shape.area(); }

function scale(p: ref Point, k: i32) i32 { return p.x * k; }

function main() i32 {
    var p = Point(1, 2);
    ref r = p;
    var s = r.sum();
    var q = p.x;
    var pair: Pair = { x: 3, y: 4 };
    var sq = Square(3);
    var shape = Shape.Circle(2);
    var m = measure(shape);
    var n = 0;
    for (var i: i32 = 0; i < 3; i = i + 1) {
        n = n + i;
    }
    return add(s, q) + describe(sq) + m + scale(p, 2) + pair.x + n;
}
)";

}  // namespace

TEST(Tooling_Lsp_References, LocalVariableIncludingAssignments) {
  EXPECT_TRUE(refersTo(kProgram, "total + 1", true,
                       {{"total: i32"},
                        {"total = total"},
                        {"total + 1"},
                        {"total += a"},
                        {"total;"}}));
  EXPECT_TRUE(
      refersTo(kProgram, "total + 1", false,
               {{"total = total"}, {"total + 1"}, {"total += a"}, {"total;"}}));
}

TEST(Tooling_Lsp_References, Parameter) {
  EXPECT_TRUE(refersTo(kProgram, "a + b", true,
                       {{"a: i32, b"}, {"a + b"}, {"a;\n    return"}}));
  // The cursor on the parameter's own declaration
  EXPECT_TRUE(refersTo(kProgram, "a: i32, b", true,
                       {{"a: i32, b"}, {"a + b"}, {"a;\n    return"}}));
  EXPECT_TRUE(refersTo(kProgram, "b;\n", false, {{"b;\n"}}));
}

TEST(Tooling_Lsp_References, Field) {
  // Point.x: the constructor's parameter `x` is not the field
  EXPECT_TRUE(refersTo(kProgram, "x: i32;", true,
                       {{"x: i32;"},
                        {"x = x;"},
                        {"x + this.y"},
                        {"x * k"},
                        {"x;\n    var pair"}}));
  // Pair.x, named in a struct literal, is a different field
  EXPECT_TRUE(
      refersTo(kProgram, "x: 3", true, {{"x: i32;", 1}, {"x: 3"}, {"x + n"}}));
}

TEST(Tooling_Lsp_References, MethodCallsThroughVarAndRef) {
  EXPECT_TRUE(refersTo(kProgram, "sum();", true, {{"sum() i32"}, {"sum();"}}));
}

TEST(Tooling_Lsp_References, InterfaceMemberGroup) {
  // An interface member and the class members implementing it are one
  // group: from either side, the references list the interface's
  // declaration, every implementation, and the calls
  std::vector<ExpectedName> group = {
      {"area() i32;"}, {"area() i32 {"}, {"area(); }"}};
  EXPECT_TRUE(refersTo(kProgram, "area() i32;", true, group));
  EXPECT_TRUE(refersTo(kProgram, "area() i32 {", true, group));
  EXPECT_TRUE(refersTo(kProgram, "area(); }", true, group));
}

TEST(Tooling_Lsp_References, Class) {
  EXPECT_TRUE(refersTo(kProgram, "class Point", true,
                       {{"Point {"}, {"Point, k"}, {"Point(1, 2)"}}));
  EXPECT_TRUE(refersTo(kProgram, "Square(3)", true,
                       {{"Square implements"}, {"Square(3)"}}));
  // An interface is named in `implements` lists and annotations
  EXPECT_TRUE(refersTo(kProgram, "interface IShape", true,
                       {{"IShape {"}, {"IShape {", 1}, {"IShape) i32"}}));
}

TEST(Tooling_Lsp_References, EnumAndVariant) {
  EXPECT_TRUE(refersTo(kProgram, "enum Shape", true,
                       {{"Shape { Circle"},
                        {"Shape) i32"},
                        {"Shape.Circle(radius)"},
                        {"Shape.Circle(2)"}}));
  EXPECT_TRUE(refersTo(kProgram, "Circle(2)", true,
                       {{"Circle(i32)"}, {"Circle(radius)"}, {"Circle(2)"}}));
}

TEST(Tooling_Lsp_References, MatchBinding) {
  EXPECT_TRUE(
      refersTo(kProgram, "radius * 3", true, {{"radius)"}, {"radius * 3"}}));
}

TEST(Tooling_Lsp_References, ForLoopVariable) {
  EXPECT_TRUE(refersTo(
      kProgram, "i < 3", true,
      {{"i: i32 = 0"}, {"i < 3"}, {"i = i + 1"}, {"i + 1"}, {"i;\n    }"}}));
}

TEST(Tooling_Lsp_References, This) {
  // `this` is not listed among a class's references
  EXPECT_TRUE(refersTo(kProgram, "Square(3)", false, {{"Square(3)"}}));
}

TEST(Tooling_Lsp_References, NothingForLiteralsAndWhitespace) {
  EXPECT_TRUE(referencesAt(kProgram, "1, 2)", true).empty());
  EXPECT_TRUE(referencesAt(kProgram, "\n\nfunction add", true).empty());
  EXPECT_TRUE(referencesAt(kProgram, "* 3", true).empty());
  EXPECT_TRUE(referencesAt(kProgram, "return total", true).empty());
}

TEST(Tooling_Lsp_References, GenericFunctionBody) {
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
    var wide = take<i64>(pool, 2);
    return take<i32>(pool, 1);
}
)";
  // Uses inside the body are found once, through the first specialization
  EXPECT_TRUE(refersTo(source, "copy;", true, {{"copy = v"}, {"copy;"}}));
  EXPECT_TRUE(refersTo(source, "v;", true, {{"v: T"}, {"v;"}}));
  EXPECT_TRUE(refersTo(source, "take<i32>", true,
                       {{"take<T>"}, {"take<i64>"}, {"take<i32>"}}));
  EXPECT_TRUE(
      refersTo(source, "Pool, v", true, {{"Pool {"}, {"Pool, v"}, {"Pool()"}}));
}

TEST(Tooling_Lsp_References, GenericClassBody) {
  std::string source = R"(
class Box<T> {
    var value: T;
    init(v: T) { this.value = v; }
    method get() T { return this.value; }
}
function main() i32 {
    var b = Box<i32>(7);
    var flag = Box<bool>(true);
    var keep = flag.get();
    return b.get();
}
)";
  EXPECT_TRUE(refersTo(source, "value; }", true,
                       {{"value: T"}, {"value = v"}, {"value; }"}}));
  EXPECT_TRUE(refersTo(source, "get();", true,
                       {{"get() T"}, {"get();"}, {"get();", 1}}));
  EXPECT_TRUE(refersTo(source, "Box<i32>", true,
                       {{"Box<T>"}, {"Box<i32>"}, {"Box<bool>"}}));
  EXPECT_TRUE(refersTo(source, "v; }", true, {{"v: T"}, {"v; }"}}));
}

TEST(Tooling_Lsp_References, ModuleQualifiedAccess) {
  std::string source = R"(
public module util {
    public function twice(v: i32) i32 { return v * 2; }
}
function main() i32 {
    return util.twice(4) + util.twice(5);
}
)";
  EXPECT_TRUE(refersTo(source, "twice(4)", true,
                       {{"twice(v: i32)"}, {"twice(4)"}, {"twice(5)"}}));
}

TEST(Tooling_Lsp_References, Lambda) {
  std::string source = R"(
function main() i32 {
    var base = 10;
    var f = (delta: i32) => i32 { return base + delta; };
    return f(1) + f(2);
}
)";
  EXPECT_TRUE(
      refersTo(source, "delta; }", true, {{"delta: i32"}, {"delta; }"}}));
  EXPECT_TRUE(refersTo(source, "base + delta", true,
                       {{"base = 10"}, {"base + delta"}}));
  EXPECT_TRUE(
      refersTo(source, "f(1)", true, {{"f = (delta: i32)"}, {"f(1)"}, {"f(2)"}}));
}

TEST(Tooling_Lsp_References, CatchBinding) {
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
        return err.code() + err.code();
    }
}
)";
  EXPECT_TRUE(refersTo(source, "err.code() +", true,
                       {{"err: IError"}, {"err.code() +"}, {"err.code();"}}));
  // The cursor on the binding's own declaration
  EXPECT_TRUE(refersTo(source, "err: IError", true,
                       {{"err: IError"}, {"err.code() +"}, {"err.code();"}}));
  EXPECT_TRUE(
      refersTo(source, "Oops();", true, {{"Oops implements"}, {"Oops();"}}));
}

TEST(Tooling_Lsp_References, MergedFiles) {
  initTestEnvironment();
  std::filesystem::create_directories("tmp");
  std::string mainPath =
      std::filesystem::absolute("tmp/references_main.sun").string();
  std::string helperPath =
      std::filesystem::absolute("tmp/references_helper.sun").string();
  std::string helperText =
      "function helper() i32 {\n    return 1;\n}\n"
      "function twice() i32 {\n    return helper() + helper();\n}\n";
  std::string mainText = "function main() i32 {\n    return helper();\n}\n";
  {
    std::ofstream file(helperPath);
    file << helperText;
  }
  {
    std::ofstream file(mainPath);
    file << mainText;
  }
  std::string canonicalMain = std::filesystem::canonical(mainPath).string();
  std::string canonicalHelper = std::filesystem::canonical(helperPath).string();

  auto driver = Driver::createForAOT("references_test");
  auto program = driver->analyzeFiles({mainPath, helperPath}, {}, {}, {});
  ASSERT_TRUE(program.ast);
  EXPECT_FALSE(program.error.has_value());

  auto references = sun::lsp::computeReferences(
      *program.ast, mainPath, mainText,
      static_cast<int>(offsetOf(mainText, "helper()")), true);
  ASSERT_EQ(references.size(), 4u);
  // Sorted by file: the helper's declaration and two calls, then main's call
  EXPECT_EQ(references[0].filePath, canonicalHelper);
  EXPECT_EQ(references[0].range.offset,
            static_cast<int>(offsetOf(helperText, "helper() i32")));
  EXPECT_EQ(rangeText(helperText, references[0]), "helper");
  EXPECT_EQ(references[0].start.line, 0);
  EXPECT_EQ(references[0].start.character, 9);
  EXPECT_EQ(references[0].end.character, 15);
  EXPECT_EQ(references[1].filePath, canonicalHelper);
  EXPECT_EQ(references[1].range.offset,
            static_cast<int>(offsetOf(helperText, "helper() +")));
  EXPECT_EQ(references[2].filePath, canonicalHelper);
  EXPECT_EQ(references[2].range.offset,
            static_cast<int>(offsetOf(helperText, "helper();")));
  EXPECT_EQ(references[3].filePath, canonicalMain);
  EXPECT_EQ(references[3].range.offset,
            static_cast<int>(offsetOf(mainText, "helper()")));
  EXPECT_EQ(references[3].start.line, 1);
  EXPECT_EQ(references[3].start.character, 11);
  std::filesystem::remove(mainPath);
  std::filesystem::remove(helperPath);
}

TEST(Tooling_Lsp_References, StdlibReferences) {
  if (getStdlibMoonImports().empty()) GTEST_SKIP() << "stdlib.moon not built";
  std::string source = R"(
using std;
function main() i64 {
    var allocator = make_heap_allocator();
    var v = Vec<i64>(allocator, 8);
    v.push(1);
    v.push(2);
    var sum: i64 = 0;
    for (var item: i64 in v) {
        sum = sum + item;
    }
    return sum;
}
)";
  // Uses in the document, plus the declaration in the library's source
  auto expectStdlib = [&](const std::string& needle,
                          const std::vector<ExpectedName>& expected,
                          const std::string& file, const std::string& name) {
    auto results = referencesAt(source, needle, true, 0, true);
    std::vector<sun::lsp::SymbolLocation> library;
    std::vector<int> got;
    for (const auto& result : results) {
      if (result.filePath == kPath) {
        got.push_back(result.range.offset);
      } else {
        library.push_back(result);
      }
    }
    std::vector<int> want;
    for (const auto& item : expected) {
      want.push_back(
          static_cast<int>(offsetOf(source, item.needle, item.occurrence)));
    }
    std::sort(want.begin(), want.end());
    EXPECT_EQ(got, want) << describe(source, results);
    ASSERT_EQ(library.size(), 1u) << describe(source, results);
    EXPECT_TRUE(
        library[0].filePath.size() > file.size() &&
        library[0].filePath.compare(library[0].filePath.size() - file.size(),
                                    file.size(), file) == 0)
        << library[0].filePath;
    EXPECT_EQ(rangeText(readFile(library[0].filePath), library[0]), name);
  };
  expectStdlib("push(1)", {{"push(1)"}, {"push(2)"}}, "stdlib/vec.sun", "push");
  expectStdlib("Vec<i64>(", {{"Vec<i64>("}}, "stdlib/vec.sun", "Vec");
  // The loop variable is declared in the document itself
  auto item = referencesAt(source, "item;", true, 0, true);
  ASSERT_EQ(item.size(), 2u) << describe(source, item);
  EXPECT_EQ(item[0].filePath, kPath);
  EXPECT_EQ(item[0].range.offset,
            static_cast<int>(offsetOf(source, "item: i64 in v")));
  EXPECT_EQ(item[1].range.offset, static_cast<int>(offsetOf(source, "item;")));
}
