// tests/tooling/lsp/test_rename.cpp — Rename symbol
//
// Each test analyzes a small program without generating code, asks to rename
// the symbol at the first byte of a distinctive snippet, and checks that the
// edit sites are exactly the expected set of name ranges.

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
#include "lsp/rename.h"

namespace {

// The file never exists on disk; nodes carry the path exactly as given
const char* kPath = "/rename_test.sun";

struct Analysis {
  std::unique_ptr<Driver> driver;
  Driver::AnalyzedProgram program;
};

Analysis analyze(const std::string& source, bool withStdlib = false) {
  initTestEnvironment();
  Analysis analysis;
  analysis.driver = Driver::createForAOT("rename_test");
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

std::optional<sun::lsp::Rename> renameAt(const std::string& source,
                                         const std::string& needle,
                                         int occurrence = 0,
                                         bool withStdlib = false) {
  size_t pos = offsetOf(source, needle, occurrence);
  if (pos == std::string::npos) return std::nullopt;
  Analysis analysis = analyze(source, withStdlib);
  if (!analysis.program.ast) {
    ADD_FAILURE() << "program did not parse: "
                  << (analysis.program.error ? analysis.program.error->what()
                                             : "");
    return std::nullopt;
  }
  return sun::lsp::computeRename(*analysis.program.ast, kPath, source,
                                 static_cast<int>(pos));
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
                     const std::vector<sun::lsp::SymbolLocation>& sites) {
  std::string text;
  for (const auto& site : sites) {
    if (!text.empty()) text += ", ";
    text += std::to_string(site.range.offset) + " (" + rangeText(source, site) +
            ")";
    if (site.filePath != kPath) text += " in " + site.filePath;
  }
  return text.empty() ? "nothing" : text;
}

// A name expected among the sites: the Nth occurrence of a snippet that
// starts with it
struct ExpectedName {
  std::string needle;
  int occurrence = 0;
};

// Renaming the symbol at `needle` edits exactly the names at `expected`,
// all in the document, and is not refused
testing::AssertionResult renames(const std::string& source,
                                 const std::string& needle,
                                 const std::vector<ExpectedName>& expected,
                                 int needleOccurrence = 0) {
  std::optional<sun::lsp::Rename> rename =
      renameAt(source, needle, needleOccurrence);
  if (!rename) {
    return testing::AssertionFailure() << "nothing to rename at " << needle;
  }
  if (!rename->refusal.empty()) {
    return testing::AssertionFailure()
           << "rename at " << needle << " refused: " << rename->refusal;
  }
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
  for (const auto& site : rename->sites) {
    if (site.filePath != kPath) {
      return testing::AssertionFailure()
             << "rename sites of " << needle << " are "
             << describe(source, rename->sites) << ", expected " << wantedText;
    }
    got.push_back(site.range.offset);
  }
  std::vector<int> want;
  for (const auto& [offset, name] : wanted) want.push_back(offset);
  if (got != want) {
    return testing::AssertionFailure()
           << "rename sites of " << needle << " are "
           << describe(source, rename->sites) << ", expected " << wantedText;
  }
  for (const auto& site : rename->sites) {
    if (rangeText(source, site) != wanted[site.range.offset]) {
      return testing::AssertionFailure()
             << "site of " << needle << " at " << site.range.offset
             << " covers '" << rangeText(source, site) << "'";
    }
    if (rename->name != wanted[site.range.offset]) {
      return testing::AssertionFailure()
             << "rename of " << needle << " is named '" << rename->name << "'";
    }
  }
  return testing::AssertionSuccess();
}

// The text with every site of the document replaced by newName
std::string applyEdits(const std::string& text,
                       const std::vector<sun::lsp::SymbolLocation>& sites,
                       const std::string& newName) {
  std::string edited = text;
  for (auto site = sites.rbegin(); site != sites.rend(); ++site) {
    if (site->filePath != kPath) continue;
    int end = site->range.endOffset.value_or(site->range.offset);
    edited.replace(site->range.offset, end - site->range.offset, newName);
  }
  return edited;
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
    const function sum() i32 { return this.x + this.y; }
}

interface IShape {
    function area() i32;
}

class Square implements IShape {
    var side: i32;
    init(side: i32) { this.side = side; }
    function area() i32 { return this.side * this.side; }
}

class Circle implements IShape {
    var r: i32;
    init(r: i32) { this.r = r; }
    function area() i32 { return this.r * this.r * 3; }
}

class Field {
    var w: i32;
    init(w: i32) { this.w = w; }
    function area() i32 { return this.w * this.w; }
}

class Pair {
    var x: i32;
    var y: i32;
}

enum Shape { Round(i32), Empty }

function add(a: i32, b: i32) i32 {
    var total: i32 = a + b;
    total = total + 1;
    total += a;
    return total;
}

function measure(s: Shape) i32 {
    var result = match s {
        Shape.Round(radius) => radius * 3,
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
    var c = Circle(1);
    var f = Field(2);
    var shape = Shape.Round(2);
    var m = measure(shape);
    var n = 0;
    for (var i: i32 = 0; i < 3; i = i + 1) {
        n = n + i;
    }
    return add(s, q) + describe(sq) + describe(c) + f.area() + sq.area() + m +
           scale(p, 2) + pair.x + n;
}
)";

}  // namespace

TEST(Tooling_Lsp_Rename, LocalVariable) {
  EXPECT_TRUE(renames(kProgram, "total + 1",
                      {{"total: i32"},
                       {"total = total"},
                       {"total + 1"},
                       {"total += a"},
                       {"total;"}}));
}

TEST(Tooling_Lsp_Rename, Parameter) {
  EXPECT_TRUE(renames(kProgram, "a + b",
                      {{"a: i32, b"}, {"a + b"}, {"a;\n    return"}}));
  EXPECT_TRUE(renames(kProgram, "a: i32, b",
                      {{"a: i32, b"}, {"a + b"}, {"a;\n    return"}}));
}

TEST(Tooling_Lsp_Rename, Field) {
  // Point.x: the constructor's parameter `x` is not the field
  EXPECT_TRUE(renames(kProgram, "x: i32;",
                      {{"x: i32;"},
                       {"x = x;"},
                       {"x + this.y"},
                       {"x * k"},
                       {"x;\n    var pair"}}));
  // Pair.x, named in a struct literal, is a different field
  EXPECT_TRUE(renames(kProgram, "x: 3", {{"x: i32;", 1}, {"x: 3"}, {"x + n"}}));
}

TEST(Tooling_Lsp_Rename, Class) {
  EXPECT_TRUE(renames(kProgram, "Point {",
                      {{"Point {"}, {"Point, k"}, {"Point(1, 2)"}}));
  EXPECT_TRUE(
      renames(kProgram, "Square(3)", {{"Square implements"}, {"Square(3)"}}));
}

TEST(Tooling_Lsp_Rename, Interface) {
  EXPECT_TRUE(renames(
      kProgram, "IShape {",
      {{"IShape {"}, {"IShape {", 1}, {"IShape {", 2}, {"IShape) i32"}}));
}

TEST(Tooling_Lsp_Rename, EnumAndVariant) {
  EXPECT_TRUE(renames(kProgram, "Shape { Round",
                      {{"Shape { Round"},
                       {"Shape) i32"},
                       {"Shape.Round(radius)"},
                       {"Shape.Round(2)"}}));
  EXPECT_TRUE(renames(kProgram, "Round(2)",
                      {{"Round(i32)"}, {"Round(radius)"}, {"Round(2)"}}));
}

TEST(Tooling_Lsp_Rename, Function) {
  EXPECT_TRUE(renames(kProgram, "add(s, q)", {{"add(a: i32"}, {"add(s, q)"}}));
}

TEST(Tooling_Lsp_Rename, InterfaceMemberGroup) {
  // The interface method, every implementing class's method, and the calls
  // through either; Field.area is an unrelated method of the same name
  std::vector<ExpectedName> group = {{"area() i32;"},
                                     {"area() i32 {"},
                                     {"area() i32 {", 1},
                                     {"area(); }"},
                                     {"area() + m"}};
  EXPECT_TRUE(renames(kProgram, "area() i32;", group));
  EXPECT_TRUE(renames(kProgram, "area(); }", group));
  EXPECT_TRUE(renames(kProgram, "area() i32 {", group));
  EXPECT_TRUE(renames(kProgram, "area() i32 {", group, 1));
  EXPECT_TRUE(renames(kProgram, "area() + m", group));
  EXPECT_TRUE(renames(kProgram, "area() i32 {",
                      {{"area() i32 {", 2}, {"area() + sq"}}, 2));
  EXPECT_TRUE(
      renames(kProgram, "area() + sq", {{"area() i32 {", 2}, {"area() + sq"}}));
}

TEST(Tooling_Lsp_Rename, ClassMethodWithoutInterface) {
  EXPECT_TRUE(renames(kProgram, "sum() i32", {{"sum() i32"}, {"sum();"}}));
}

TEST(Tooling_Lsp_Rename, BuiltinInterfaceMemberRefused) {
  std::string source = R"(
class Oops implements IError {
    init() {}
    function code() i32 { return 1; }
    function message() static_ptr<u8> { return "oops"; }
}
function main() i32 {
    var e = Oops();
    return e.code();
}
)";
  std::optional<sun::lsp::Rename> rename = renameAt(source, "code() i32");
  ASSERT_TRUE(rename);
  EXPECT_FALSE(rename->refusal.empty());
  EXPECT_NE(rename->refusal.find("IError"), std::string::npos);
  // The constructor is the class's own
  rename = renameAt(source, "init()");
  ASSERT_TRUE(rename);
  EXPECT_TRUE(rename->refusal.empty()) << rename->refusal;
}

TEST(Tooling_Lsp_Rename, RoundTrip) {
  std::optional<sun::lsp::Rename> rename = renameAt(kProgram, "area() i32;");
  ASSERT_TRUE(rename);
  std::string renamed = applyEdits(kProgram, rename->sites, "surface");
  // Only Field.area, which implements nothing, keeps its name
  auto count = [](const std::string& text, const std::string& word) {
    int n = 0;
    for (size_t at = text.find(word); at != std::string::npos;
         at = text.find(word, at + 1)) {
      ++n;
    }
    return n;
  };
  EXPECT_EQ(count(renamed, "area"), 2) << renamed;
  EXPECT_EQ(count(renamed, "surface"), 5) << renamed;
  EXPECT_NE(renamed.find("f.area()"), std::string::npos);
  Analysis analysis = analyze(renamed);
  ASSERT_TRUE(analysis.program.ast);
  EXPECT_FALSE(analysis.program.error.has_value())
      << analysis.program.error->what();

  // Renaming back at the same place restores the text
  std::optional<sun::lsp::Rename> back =
      sun::lsp::computeRename(*analysis.program.ast, kPath, renamed,
                              static_cast<int>(offsetOf(renamed, "surface")));
  ASSERT_TRUE(back);
  EXPECT_EQ(back->name, "surface");
  EXPECT_EQ(applyEdits(renamed, back->sites, "area"), kProgram);
}

TEST(Tooling_Lsp_Rename, PrepareRename) {
  std::string source = kProgram;
  Analysis analysis = analyze(source);
  ASSERT_TRUE(analysis.program.ast);
  int start = static_cast<int>(offsetOf(source, "total + 1"));
  // Start, middle and end of the identifier
  for (int offset : {start, start + 2, start + 5}) {
    std::optional<sun::lsp::Rename> rename =
        sun::lsp::computeRename(*analysis.program.ast, kPath, source, offset);
    ASSERT_TRUE(rename) << "at " << offset;
    EXPECT_EQ(rename->name, "total");
    std::optional<sun::lsp::SymbolLocation> site =
        sun::lsp::siteAt(*rename, kPath, offset);
    ASSERT_TRUE(site) << "at " << offset;
    EXPECT_EQ(site->range.offset, start);
    EXPECT_EQ(*site->range.endOffset, start + 5);
    EXPECT_EQ(site->start.character, site->end.character - 5);
  }
  // Not on a name: literals, whitespace, operators, keywords
  for (const char* needle : {"1, 2)", "\n\nfunction add", "* 3", "return total",
                             "class Point", "interface IShape"}) {
    EXPECT_FALSE(renameAt(source, needle)) << needle;
  }
  // `this` is not a symbol of its own
  EXPECT_FALSE(renameAt(source, "this.side * this.side"));
}

TEST(Tooling_Lsp_Rename, NewName) {
  EXPECT_EQ(sun::lsp::checkNewName("fooBar"), "");
  EXPECT_EQ(sun::lsp::checkNewName("x_1"), "");
  EXPECT_EQ(sun::lsp::checkNewName("Point2"), "");
  EXPECT_NE(sun::lsp::checkNewName(""), "");
  EXPECT_NE(sun::lsp::checkNewName("class"), "");
  EXPECT_NE(sun::lsp::checkNewName("function"), "");
  EXPECT_NE(sun::lsp::checkNewName("1abc"), "");
  EXPECT_NE(sun::lsp::checkNewName("a-b"), "");
  EXPECT_NE(sun::lsp::checkNewName("a b"), "");
  EXPECT_NE(sun::lsp::checkNewName("a.b"), "");
  EXPECT_NE(sun::lsp::checkNewName(" a"), "");
}

TEST(Tooling_Lsp_Rename, GenericClassBody) {
  std::string source = R"(
class Box<T> {
    var value: T;
    init(v: T) { this.value = v; }
    function get() T { return this.value; }
}
function main() i32 {
    var b = Box<i32>(7);
    var flag = Box<bool>(true);
    var keep = flag.get();
    return b.get();
}
)";
  EXPECT_TRUE(
      renames(source, "value; }", {{"value: T"}, {"value = v"}, {"value; }"}}));
  EXPECT_TRUE(
      renames(source, "get();", {{"get() T"}, {"get();"}, {"get();", 1}}));
  EXPECT_TRUE(
      renames(source, "Box<i32>", {{"Box<T>"}, {"Box<i32>"}, {"Box<bool>"}}));
}

TEST(Tooling_Lsp_Rename, MergedFiles) {
  initTestEnvironment();
  std::filesystem::create_directories("tmp");
  std::string mainPath =
      std::filesystem::absolute("tmp/rename_main.sun").string();
  std::string helperPath =
      std::filesystem::absolute("tmp/rename_helper.sun").string();
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

  auto driver = Driver::createForAOT("rename_test");
  auto program = driver->analyzeFiles({mainPath, helperPath}, {}, {}, {});
  ASSERT_TRUE(program.ast);
  EXPECT_FALSE(program.error.has_value());

  int offset = static_cast<int>(offsetOf(mainText, "helper()"));
  std::optional<sun::lsp::Rename> rename =
      sun::lsp::computeRename(*program.ast, mainPath, mainText, offset);
  ASSERT_TRUE(rename);
  EXPECT_TRUE(rename->refusal.empty()) << rename->refusal;
  EXPECT_EQ(rename->name, "helper");
  ASSERT_EQ(rename->sites.size(), 4u);
  // Sorted by file: the helper's declaration and two calls, then main's call
  EXPECT_EQ(rename->sites[0].filePath, canonicalHelper);
  EXPECT_EQ(rename->sites[0].range.offset,
            static_cast<int>(offsetOf(helperText, "helper() i32")));
  EXPECT_EQ(rename->sites[0].start.line, 0);
  EXPECT_EQ(rename->sites[0].start.character, 9);
  EXPECT_EQ(rename->sites[0].end.character, 15);
  EXPECT_EQ(rename->sites[1].filePath, canonicalHelper);
  EXPECT_EQ(rename->sites[2].filePath, canonicalHelper);
  EXPECT_EQ(rename->sites[3].filePath, canonicalMain);
  EXPECT_EQ(rename->sites[3].range.offset, offset);
  EXPECT_EQ(rename->sites[3].start.line, 1);
  EXPECT_EQ(rename->sites[3].start.character, 11);

  std::optional<sun::lsp::SymbolLocation> site =
      sun::lsp::siteAt(*rename, mainPath, offset + 3);
  ASSERT_TRUE(site);
  EXPECT_EQ(site->filePath, canonicalMain);
  std::filesystem::remove(mainPath);
  std::filesystem::remove(helperPath);
}

TEST(Tooling_Lsp_Rename, StdlibInterfaceMemberRefused) {
  if (getStdlibMoonImports().empty()) GTEST_SKIP() << "stdlib.moon not built";
  // The member group of `next` reaches the stdlib's IIterator, whose own
  // uses cannot be edited, so the rename is refused
  std::string source = R"(
using sun;
class Counter {
    var n: i64;
    init() { this.n = 0; }
}
class CounterIter implements IIterator<i64, Counter> {
    init() {}
    function next(container: ref Counter) Option<i64> {
        container.n = container.n + 1;
        return Option.Some(container.n);
    }
}
function main() i64 {
    var c = Counter();
    var it = CounterIter();
    var v = it.next(c);
    return 0;
}
)";
  std::optional<sun::lsp::Rename> rename =
      renameAt(source, "next(container", 0, true);
  ASSERT_TRUE(rename);
  EXPECT_NE(rename->refusal.find("library"), std::string::npos)
      << rename->refusal;
  // The class's own sites are still listed, in the document
  for (const auto& site : rename->sites) {
    if (site.filePath == kPath) EXPECT_EQ(rangeText(source, site), "next");
  }
}

TEST(Tooling_Lsp_Rename, LibrarySymbolRefused) {
  if (getStdlibMoonImports().empty()) GTEST_SKIP() << "stdlib.moon not built";
  std::string source = R"(
using sun;
function main() i64 {
    var allocator = make_heap_allocator();
    var v = Vec<i64>(allocator, 8);
    v.push(1);
    var item: i64 = 0;
    return item;
}
)";
  std::optional<sun::lsp::Rename> rename =
      renameAt(source, "Vec<i64>", 0, true);
  ASSERT_TRUE(rename);
  EXPECT_EQ(rename->name, "Vec");
  EXPECT_NE(rename->refusal.find("library"), std::string::npos)
      << rename->refusal;
  rename = renameAt(source, "push(1)", 0, true);
  ASSERT_TRUE(rename);
  EXPECT_FALSE(rename->refusal.empty());
  // A local in the document is still renamable
  rename = renameAt(source, "item;", 0, true);
  ASSERT_TRUE(rename);
  EXPECT_TRUE(rename->refusal.empty()) << rename->refusal;
  EXPECT_EQ(rename->sites.size(), 2u);
}
