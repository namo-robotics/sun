// tests/modules/test_access_modifiers.cpp
// Access modifiers: `public` is the only keyword, everything else is private.
// Privacy is module-scoped: a private item is reachable from its owning
// module and that module's descendants.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "ast_deserializer.h"
#include "ast_serializer.h"
#include "execution_utils.h"
#include "formatter.h"
#include "lexer.h"
#include "moon_builder.h"
#include "moon_import.h"
#include "parser.h"

namespace {

// A module exposing one public and one private member of every kind
const char* kLib = R"(
  public module m {
      public class Box {
          public var v: i32;
          var secret: i32;
          public function init(v: i32) { this.v = v; this.secret = v * 2; }
          function hidden() i32 { return this.secret; }
          public function reveal() i32 { return helper() + this.hidden(); }
      }
      class Hidden { public var x: i32; public function init() { this.x = 1; } }
      function helper() i32 { return 100; }
      public function make(v: i32) Box { return Box(v); }
      enum Secret { A, B }
      public enum Color { Red, Green }
      var counter: i32 = 7;
      public var visible: i32 = 8;
      module inner { public function f() i32 { return 1; } }
      public module pubinner { public function g() i32 { return 2; } }
  }
)";

std::string program(const std::string& body) {
  return std::string(kLib) + "\nfunction main() i32 {\n" + body + "\n}\n";
}

std::string usingProgram(const std::string& body) {
  return std::string(kLib) + "\nusing m;\nfunction main() i32 {\n" + body +
         "\n}\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Lexer / parser
// ---------------------------------------------------------------------------

TEST(Modules_AccessModifiers, lexer_public_is_keyword_private_is_identifier) {
  std::istringstream in("public publicity private");
  Lexer lexer(in);
  EXPECT_EQ(lexer.getNextToken().kind, TokenKind::PUBLIC);
  EXPECT_EQ(lexer.getNextToken().kind, TokenKind::IDENTIFIER);
  EXPECT_EQ(lexer.getNextToken().kind, TokenKind::IDENTIFIER);
}

TEST(Modules_AccessModifiers, parser_accepts_public_on_every_item_kind) {
  auto parser = Parser::createStringParser(R"(
    public module a.b {
        public class C { public var x: i32; var y: i32;
                         public function init() {} function h() void {} }
        public interface I { public var f: i32; public function m() i32;
                             function d() i32 { return 1; } }
        public enum E { A, B }
        public var g: i32 = 1;
        public function fn() i32 { return 1; }
        public extern "C" function puts(s: raw_ptr<u8>) i32;
        public declare function fwd(a: i32) i32;
        public declare Alias = i32;
        public module inner { public function f() i32 { return 1; } }
    }
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1u);
  auto* outer = static_cast<const ModuleAST*>(ast->getBody()[0].get());
  EXPECT_TRUE(outer->isPublic());
  // `public module a.b` makes every synthesized level public
  auto* inner = static_cast<const ModuleAST*>(outer->getBody().getBody()[0].get());
  EXPECT_EQ(inner->getName(), "b");
  EXPECT_TRUE(inner->isPublic());
  for (const auto& stmt : inner->getBody().getBody()) {
    EXPECT_TRUE(stmt->isPublic()) << stmt->toString();
  }
  auto* cls = static_cast<const ClassDefinitionAST*>(
      inner->getBody().getBody()[0].get());
  EXPECT_EQ(cls->getFields()[0].visibility, sun::Visibility::Public);
  EXPECT_EQ(cls->getFields()[1].visibility, sun::Visibility::Private);
  EXPECT_TRUE(cls->getMethods()[0].function->isPublic());
  EXPECT_FALSE(cls->getMethods()[1].function->isPublic());
}

TEST(Modules_AccessModifiers, parser_rejects_public_inside_function_body) {
  EXPECT_THROW(compileString(R"(
    function main() i32 { public var x: i32 = 1; return x; }
  )"), SunError);
}

TEST(Modules_AccessModifiers, parser_rejects_public_on_non_declarations) {
  EXPECT_THROW(compileString("public using m;\nfunction main() i32 { return 0; }"),
               SunError);
  EXPECT_THROW(compileString(R"(
    public module m { public public function f() i32 { return 1; } }
    function main() i32 { return 0; }
  )"), SunError);
}

TEST(Modules_AccessModifiers, formatter_round_trips_public) {
  std::string src =
      "public module m {\n"
      "  public class C {\n"
      "    public var x: i32;\n"
      "    var y: i32;\n"
      "    public function init() {}\n"
      "  }\n"
      "  public interface I {\n"
      "    public function m() i32;\n"
      "  }\n"
      "  public enum E { A, B }\n"
      "  public var g: i32 = 1;\n"
      "  public extern \"C\" function puts(s: raw_ptr<u8>) i32;\n"
      "  public declare Alias = i32;\n"
      "  public function f() i32 {\n"
      "    return 1;\n"
      "  }\n"
      "}\n";
  EXPECT_EQ(sun::formatSource(src), src);
}

TEST(Modules_AccessModifiers, serialization_round_trips_visibility) {
  auto parser = Parser::createStringParser(R"(
    public module m {
        public class C { public var x: i32; var y: i32;
                         public function init() {} function h() void {} }
        enum E { A }
    }
  )");
  auto ast = parser.parseProgram();
  sun::serialization::ASTSerializer serializer;
  sun::serialization::ASTDeserializer deserializer;
  auto proto = serializer.serialize(*ast->getBody()[0]);
  auto back = deserializer.deserialize(proto);
  ASSERT_NE(back, nullptr);
  auto* mod = static_cast<const ModuleAST*>(back.get());
  EXPECT_TRUE(mod->isPublic());
  auto* cls = static_cast<const ClassDefinitionAST*>(
      mod->getBody().getBody()[0].get());
  EXPECT_TRUE(cls->isPublic());
  EXPECT_EQ(cls->getFields()[0].visibility, sun::Visibility::Public);
  EXPECT_EQ(cls->getFields()[1].visibility, sun::Visibility::Private);
  EXPECT_TRUE(cls->getMethods()[0].function->isPublic());
  EXPECT_FALSE(cls->getMethods()[1].function->isPublic());
  EXPECT_FALSE(mod->getBody().getBody()[1]->isPublic());
}

// ---------------------------------------------------------------------------
// Same-module and public access
// ---------------------------------------------------------------------------

TEST(Modules_AccessModifiers, public_api_is_reachable_from_outside) {
  auto value = executeString(program(
      "var b = m.make(3); return b.reveal() + b.v + m.pubinner.g() + m.visible;"));
  EXPECT_EQ(value, 106 + 3 + 2 + 8);
}

TEST(Modules_AccessModifiers, private_items_reachable_within_module) {
  auto value = executeString(R"(
    public module m {
        class Node { public var x: i32; public function init(x: i32) { this.x = x; } }
        function helper() i32 { return 5; }
        var counter: i32 = 2;
        enum Kind { A }
        public function api() i32 {
            var n = Node(helper());
            var k = Kind.A;
            return n.x + counter;
        }
    }
    function main() i32 { return m.api(); }
  )");
  EXPECT_EQ(value, 7);
}

TEST(Modules_AccessModifiers, root_level_items_are_reachable_everywhere) {
  auto value = executeString(R"(
    class Root { var v: i32; function init(v: i32) { this.v = v; } function get() i32 { return this.v; } }
    function helper() i32 { return 1; }
    public module m {
        public function api() i32 { var r = Root(4); return r.get() + r.v + helper(); }
    }
    function main() i32 { var r = Root(1); return m.api() + r.v; }
  )");
  EXPECT_EQ(value, 10);
}

TEST(Modules_AccessModifiers, child_module_sees_parent_private_items) {
  auto value = executeString(R"(
    public module a {
        function h() i32 { return 3; }
        class P { public var x: i32; public function init() { this.x = 4; } }
        public module b {
            public function f() i32 { var p = P(); return h() + p.x; }
        }
    }
    function main() i32 { return a.b.f(); }
  )");
  EXPECT_EQ(value, 7);
}

// ---------------------------------------------------------------------------
// Denied access, qualified and via `using`
// ---------------------------------------------------------------------------

TEST(Modules_AccessModifiers, private_field_denied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      compileString(program("var b = m.make(1); return b.secret;")),
      "'secret' is private to class 'Box' in module 'm'");
}

TEST(Modules_AccessModifiers, private_field_assignment_denied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      compileString(program("var b = m.make(1); b.secret = 3; return 0;")),
      "'secret' is private to class 'Box' in module 'm'");
}

TEST(Modules_AccessModifiers, private_method_denied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      compileString(program("var b = m.make(1); return b.hidden();")),
      "'hidden' is private to class 'Box' in module 'm'");
}

TEST(Modules_AccessModifiers, private_function_denied_qualified_and_via_using) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(program("return m.helper();")),
                                "function 'helper' is private to module 'm'");
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(usingProgram("return helper();")),
                                "function 'helper' is private to module 'm'");
}

TEST(Modules_AccessModifiers, private_class_denied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      compileString(usingProgram("var h = Hidden(); return h.x;")),
      "class 'Hidden' is private to module 'm'");
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      compileString(usingProgram("var h: Hidden = Hidden(); return 0;")),
      "class 'Hidden' is private to module 'm'");
}

TEST(Modules_AccessModifiers, private_enum_denied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      compileString(usingProgram("var s = Secret.A; return 0;")),
      "enum 'Secret' is private to module 'm'");
}

TEST(Modules_AccessModifiers, private_global_denied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(program("return m.counter;")),
                                "variable 'counter' is private to module 'm'");
}

TEST(Modules_AccessModifiers, private_nested_module_denied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(program("return m.inner.f();")),
                                "module 'inner' is private to module 'm'");
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      compileString(std::string(kLib) +
                    "using m.inner;\nfunction main() i32 { return f(); }"),
      "module 'inner' is private to module 'm'");
}

TEST(Modules_AccessModifiers, module_reopened_with_conflicting_visibility_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    public module m { public function f() i32 { return 1; } }
    module m { public function g() i32 { return 2; } }
    function main() i32 { return m.f() + m.g(); }
  )"), "all declarations of a module must agree");
}

// ---------------------------------------------------------------------------
// Constructors, deinit, operators, struct literals
// ---------------------------------------------------------------------------

TEST(Modules_AccessModifiers, private_init_with_public_factory) {
  auto value = executeString(R"(
    public module m {
        public class Token {
            public var id: i32;
            function init(id: i32) { this.id = id; }
            public function make(id: i32) Token { return Token(id); }
        }
        public function issue(id: i32) Token { return Token(id); }
    }
    function main() i32 { var t = m.issue(9); return t.id; }
  )");
  EXPECT_EQ(value, 9);
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    public module m {
        public class Token { public var id: i32; function init(id: i32) { this.id = id; } }
    }
    function main() i32 { var t = m.Token(9); return t.id; }
  )"), "'init' is private to class 'Token' in module 'm'");
}

TEST(Modules_AccessModifiers, deinit_is_always_callable) {
  auto value = executeString(R"(
    public module m {
        public class R { public var v: i32; public function init() { this.v = 1; }
                         function deinit() void { this.v = 0; } }
    }
    function main() i32 { var r = m.R(); r.deinit(); return r.v; }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_AccessModifiers, private_index_operator_denied) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    public module m {
        public class Arr { public var v: i32; public function init() { this.v = 1; }
                           function __index__(i: i64) i32 { return this.v; } }
    }
    function main() i32 { var a = m.Arr(); return a[0]; }
  )"), "'__index__' is private to class 'Arr' in module 'm'");
}

TEST(Modules_AccessModifiers, struct_literal_needs_accessible_fields) {
  auto value = executeString(R"(
    public module m {
        public class P { public var x: i32; public var y: i32; }
    }
    using m;
    function main() i32 { var p: P = { x: 1, y: 2 }; return p.x + p.y; }
  )");
  EXPECT_EQ(value, 3);
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    public module m {
        public class P { public var x: i32; var y: i32; }
    }
    using m;
    function main() i32 { var p: P = { x: 1, y: 2 }; return p.x; }
  )"), "'y' is private to class 'P' in module 'm'");
}

// ---------------------------------------------------------------------------
// Generics instantiated across a module boundary
// ---------------------------------------------------------------------------

TEST(Modules_AccessModifiers, generic_bodies_keep_their_module_context) {
  auto value = executeString(R"(
    public module m {
        function helper() i32 { return 100; }
        class Node<T> { public var v: T; public function init(v: T) { this.v = v; } }
        public class Wrap<T> {
            var n: Node<T>;
            public function init(v: T) { this.n = Node<T>(v); }
            public function get() T { return this.n.v + helper(); }
            public function get_as<U>() U { return helper(); }
        }
        public function id<T>(x: T) T { helper(); return x; }
    }
    using m;
    function main() i32 {
        var w = Wrap<i32>(5);
        return w.get() + id<i32>(1) + w.get_as<i32>();
    }
  )");
  EXPECT_EQ(value, 105 + 1 + 100);
}

// ---------------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------------

TEST(Modules_AccessModifiers, public_interface_member_needs_public_implementation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    public module m {
        public interface IShow { public function show() i32; }
        public class C implements IShow { public function init() {} function show() i32 { return 1; } }
    }
    function main() i32 { return 0; }
  )"), "implements public member 'IShow.show' and must be public");
}

TEST(Modules_AccessModifiers, private_interface_member_only_reachable_in_module) {
  auto value = executeString(R"(
    public module m {
        public interface IShow { function show() i32; }
        public class C implements IShow { public function init() {} public function show() i32 { return 4; } }
        public function make() C { return C(); }
        public function twice(i: ref IShow) i32 { return i.show() * 2; }
    }
    function main() i32 { var c = m.make(); return m.twice(c); }
  )");
  EXPECT_EQ(value, 8);
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    public module m {
        public interface IShow { function show() i32; }
        public class C implements IShow { public function init() {} public function show() i32 { return 4; } }
        public function make() C { return C(); }
    }
    using m;
    function via(i: ref IShow) i32 { return i.show(); }
    function main() i32 { var c = make(); return via(c); }
  )"), "'show' is private to interface 'IShow' in module 'm'");
}

TEST(Modules_AccessModifiers, inherited_interface_field_keeps_interface_visibility) {
  auto value = executeString(R"(
    public module m {
        public interface IHas { public var n: i32; }
        public class C implements IHas { public function init() { this.n = 6; } }
        public function make() C { return C(); }
    }
    function main() i32 { var c = m.make(); return c.n; }
  )");
  EXPECT_EQ(value, 6);
}

// ---------------------------------------------------------------------------
// .moon bundles: private items are carried but hidden; roots must be public
// ---------------------------------------------------------------------------

namespace {
namespace fs = std::filesystem;

fs::path writeLib(const std::string& name, const std::string& src) {
  fs::path dir = fs::temp_directory_path() / "sun_access_moon_test";
  fs::create_directories(dir);
  fs::path libSrc = dir / (name + ".sun");
  std::ofstream out(libSrc);
  out << src;
  return libSrc;
}
}  // namespace

TEST(Modules_AccessModifiers, moon_hides_private_items_but_generics_still_work) {
  initTestEnvironment();
  fs::path libSrc = writeLib("acclib", R"(
    public module acclib {
        function helper() i32 { return 10; }
        public class Counter<T> {
            var n: i32;
            public function init() { this.n = helper(); }
            public function bump() i32 { this.n = this.n + helper(); return this.n; }
        }
        public function make() Counter<i32> { return Counter<i32>(); }
    }
  )");
  fs::path moonPath = libSrc.parent_path() / "acclib.moon";
  sun::MoonBuilder::build(libSrc.string(), moonPath);

  {
    auto driver = Driver::createForJIT("moon_main_ok");
    driver->setMoonImports({sun::MoonImport(moonPath.string())});
    auto value = driver->executeString(R"(
      using acclib;
      function main() i32 { var c = Counter<i64>(); return c.bump(); }
    )");
    EXPECT_EQ(value, 20);
  }
  {
    auto driver = Driver::createForJIT("moon_main_denied");
    driver->setMoonImports({sun::MoonImport(moonPath.string())});
    EXPECT_SUN_ERROR_WITH_MESSAGE(driver->executeString(R"(
      using acclib;
      function main() i32 { return helper(); }
    )"), "function 'helper' is private to module 'acclib'");
  }
  {
    auto driver = Driver::createForJIT("moon_main_field");
    driver->setMoonImports({sun::MoonImport(moonPath.string())});
    EXPECT_SUN_ERROR_WITH_MESSAGE(driver->executeString(R"(
      using acclib;
      function main() i32 { var c = make(); return c.n; }
    )"), "'n' is private to class");
  }
}

TEST(Modules_AccessModifiers, moon_rejects_private_root_module) {
  initTestEnvironment();
  fs::path libSrc = writeLib("privroot", R"(
    module privroot { public function f() i32 { return 1; } }
  )");
  fs::path moonPath = libSrc.parent_path() / "privroot.moon";
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::MoonBuilder::build(libSrc.string(), moonPath),
                                "top-level module 'privroot' must be declared 'public'");
}

// The stdlib's own internals are hidden from user code
TEST(Modules_AccessModifiers, stdlib_internals_are_private) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(R"(
    using sun;
    function main() i32 {
        var allocator = make_heap_allocator();
        var v = Vec<i32>(allocator, 4);
        return v.size_;
    }
  )", /*includeStdlib=*/true), "'size_' is private to class 'sun.Vec<i32>' in module 'sun'");
}
