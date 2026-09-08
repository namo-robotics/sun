// Regression coverage for generic substitution and module lookup (#257–#260).

#include <gtest/gtest.h>
#include <llvm/Support/Program.h>
#include <unistd.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream text;
  text << input.rdbuf();
  return text.str();
}

// Each compiler invocation has a deadline so a recursion bug fails the test.
class Modules_GenericRegressions : public ::testing::Test {
 protected:
  std::filesystem::path dir;

  void SetUp() override {
    ASSERT_TRUE(std::filesystem::exists("build/sun"));
    dir = std::filesystem::path("tmp") /
          ("generic_regression_" + std::to_string(getpid()) + "_" +
           ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(dir);
  }

  void TearDown() override { std::filesystem::remove_all(dir); }

  void write(const std::string& name, const std::string& source) {
    std::ofstream(dir / name) << source;
  }

  ::testing::AssertionResult run(const std::string& command,
                                 bool expectSuccess = true,
                                 const std::string& diagnostic = "") {
    const auto log = dir / "log";
    const std::string shell =
        "exec " + command + " > " + log.string() + " 2>&1";
    std::string error;
    const int status = llvm::sys::ExecuteAndWait(
        "/bin/sh", {"sh", "-c", shell}, std::nullopt, {},
        /*SecondsToWait=*/30, /*MemoryLimit=*/0, &error);
    const std::string output = readFile(log);
    if (status < 0) {
      return ::testing::AssertionFailure() << command << "\n"
                                           << output << "\n"
                                           << error;
    }
    if ((status == 0) != expectSuccess ||
        (!diagnostic.empty() && output.find(diagnostic) == std::string::npos)) {
      return ::testing::AssertionFailure() << command << "\n"
                                           << output << "\nExit: " << status;
    }
    return ::testing::AssertionSuccess();
  }

  void checkProgram(const std::string& name) {
    const auto source = (dir / name).string();
    ASSERT_TRUE(run("build/sun " + source));
    const auto binary = (dir / "app").string();
    ASSERT_TRUE(run("build/sun -c -o " + binary + " " + source));
    ASSERT_TRUE(run(binary));
  }

  void buildLibrary(const std::string& source) {
    write("lib.sun", source);
    ASSERT_TRUE(run("build/sun --emit-moon -o " + (dir / "lib.moon").string() +
                    " " + (dir / "lib.sun").string()));
  }
};

const std::string supportClass = R"(
/** Holds the value used to verify the indirect clone call. */
public class TypeSupport<T> {
  public var n: i32;
  init(n: i32) { this.n = n; }
  /** Returns another support object with the same value. */
  public const method clone() TypeSupport<T> { return TypeSupport<T>(this.n); }
}
)";

const std::string subscriberClass = R"(
/** Constructs support objects through a generic method. */
public class Subscriber {
  init() {}
  /** Calls a method on the consumer's specialized support type. */
  public method make<R>(support: TypeSupport<R>) TypeSupport<R> {
    return support.clone();
  }
}
)";

const std::string consumer = R"(
class Msg { public var n: i32 = 0; init() {} }
function main() i32 {
  var s = lib.Subscriber();
  var result = s.make<Msg>(lib.TypeSupport<Msg>(7));
  return result.n - 7;
}
manifest { libraries: ["lib.moon"] }
)";

TEST_F(Modules_GenericRegressions, ImportedMethodDependencies) {
  for (bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    ASSERT_NO_FATAL_FAILURE(buildLibrary(
        "/** Defines library generic templates. */\npublic module lib {" +
        (reverse ? supportClass + subscriberClass
                 : subscriberClass + supportClass) +
        "}"));

    write("consumer.sun", consumer);
    ASSERT_NO_FATAL_FAILURE(checkProgram("consumer.sun"));
  }
}

TEST_F(Modules_GenericRegressions, ModuleGlobals) {
  const std::string library = R"(
/** Owns the globals referenced by generic bodies. */
public module lib {
  public var VALUE: i32 = 7;
  /** Requires initialization when the library is loaded. */
  public class State {
    public var n: i32;
    init(n: i32) { this.n = n; }
  }
  public var STATE: State = State(11);
  /** Reads a global from a generic class. */
  public class Box<T> {
    init() {}
    /** Returns this module's value. */
    public method value() i32 { return VALUE + STATE.n - 11; }
  }
  /** Reads a global from a generic method. */
  public class Factory {
    init() {}
    /** Returns this module's value. */
    public method value<T>() i32 { return VALUE + STATE.n - 11; }
  }
}
)";
  const std::string app = R"(
/** Provides a conflicting name at the instantiation site. */
public module app {
  public var VALUE: i32 = 99;
  /** Requires initialization in the consumer. */
  public class State {
    public var n: i32;
    init(n: i32) { this.n = n; }
  }
  public var STATE: State = State(13);
  /** Checks that templates use their defining module's global. */
  public function check() i32 {
    var b = lib.Box<i32>();
    var f = lib.Factory();
    return b.value() + f.value<i32>() + VALUE + STATE.n - 126;
  }
}
function main() i32 { return app.check(); }
)";
  write("local.sun", library + app);
  ASSERT_NO_FATAL_FAILURE(checkProgram("local.sun"));
  ASSERT_NO_FATAL_FAILURE(buildLibrary(library));

  write("consumer.sun", app + "manifest { libraries: [\"lib.moon\"] }");
  ASSERT_NO_FATAL_FAILURE(checkProgram("consumer.sun"));
}

TEST_F(Modules_GenericRegressions, ModuleFunctions) {
  const std::string library = R"(
/** Supplies generic functions called within and outside the module. */
public module lib {
  /** Returns its input. */
  public function identity<T>(value: T) T { return value; }
  /** Calls a sibling with explicit and inferred type arguments. */
  public function twice<T>(value: T) T {
    return identity<T>(value) + identity(value);
  }
  /** Calls generic siblings from a plain function. */
  public function same_file() i32 { return identity<i32>(7) + identity(7); }
}
)";
  const std::string app = R"(
/** Reopens the library module in another source file. */
public module lib {
  /** Calls the sibling functions in the other file. */
  public function reopened() i32 { return twice<i32>(7) + twice(7); }
}
function main() i32 {
  return lib.identity<i32>(7) + lib.identity(7) + lib.same_file() +
         lib.reopened() - 56;
}
manifest { source_files: ["lib.sun"] }
)";
  write("lib.sun", library);
  write("app.sun", app);
  ASSERT_NO_FATAL_FAILURE(checkProgram("app.sun"));
  ASSERT_NO_FATAL_FAILURE(buildLibrary(library));

  write("consumer.sun", R"(
using lib;
/** Provides the same generic name in a different module. */
public module other {
  /** Returns a distinct result to check qualified resolution. */
  public function twice<T>(value: T) T { return value + value + value; }
}
function main() i32 {
  return lib.twice<i32>(7) + lib.twice(7) + twice<i32>(7) + twice(7) +
         other.twice<i32>(7) + other.twice(7) - 98;
}
manifest { libraries: ["lib.moon"] }
)");
  ASSERT_NO_FATAL_FAILURE(checkProgram("consumer.sun"));
}

TEST_F(Modules_GenericRegressions, ModuleContainerFunction) {
  write("container.sun", R"(
using std;
/** Provides a generic operation on a standard container. */
public module lib {
  /** Moves an element out of the vector. */
  public function rem<T>(v: ref Vec<T>, index: i64) T throws IError {
    return v.take(index);
  }
  /** Calls the sibling function from another generic body. */
  public function nested<T>(v: ref Vec<T>) T throws IError { return rem<T>(v, 2); }
  /** Exercises explicit, inferred and nested generic calls. */
  public function check() i32 throws IError {
    var a = make_heap_allocator();
    var v = Vec<i64>(a, 3);
    v.push(1); v.push(2); v.push(3);
    var first = rem<i64>(v, 0);
    var second = rem(v, 1);
    var third = nested<i64>(v);
    if (first == 1 and second == 2 and third == 3 and v.size() == 3) { return 0; }
    return 1;
  }
}
function main() i32 throws IError { return lib.check(); }
manifest { libraries: ["stdlib.moon"] }
)");
  ASSERT_NO_FATAL_FAILURE(checkProgram("container.sun"));
}

TEST_F(Modules_GenericRegressions, PrivateModuleFunction) {
  write("private.sun", R"(
/** Keeps a generic function private. */
public module lib {
  function hidden<T>(value: T) T { return value; }
}
function main() i32 { return lib.hidden<i32>(0); }
)");
  ASSERT_TRUE(
      run("build/sun " + (dir / "private.sun").string(), false, "private"));
}

// Rename only the parameter inside one class, leaving its callers unchanged.
void renameParameter(std::string& source, const std::string& className,
                     const std::string& nextClass, char replacement) {
  const auto begin = source.find("public class " + className + "<T>");
  const auto end = source.find("public class " + nextClass, begin);
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  for (size_t i = begin; i < end; ++i) {
    if (source[i] == 'T' &&
        (i == 0 || !std::isalnum(static_cast<unsigned char>(source[i - 1]))) &&
        (i + 1 == source.size() ||
         !std::isalnum(static_cast<unsigned char>(source[i + 1])))) {
      source[i] = replacement;
    }
  }
}

TEST_F(Modules_GenericRegressions, NestedTypeArguments) {
  ASSERT_TRUE(std::filesystem::exists("build/stdlib.moon"));
  const std::string original =
      readFile("tests/programs/generic_regressions/nested.sun");
  ASSERT_FALSE(original.empty());
  for (int variant = 0; variant < 4; ++variant) {
    SCOPED_TRACE(variant);
    std::string source = original;
    if (variant == 1) {
      ASSERT_NO_FATAL_FAILURE(renameParameter(source, "Sample", "Reader", 'S'));
    }
    if (variant == 2) {
      ASSERT_NO_FATAL_FAILURE(
          renameParameter(source, "Reader", "Subscriber", 'R'));
    }
    if (variant == 3) {
      const auto replace = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = source.find(from, pos)) != std::string::npos) {
          source.replace(pos, from.size(), to);
          pos += to.size();
        }
      };
      replace("Option<T>", "Vec<T>");
      replace("var none: Vec<T> = Option.None;", "var none = Vec<T>(a, 1);");
      replace("out.push(Sample<T>(Info(), Option.Some(v)));",
              "var some = Vec<T>(a, 1); some.push(v); "
              "out.push(Sample<T>(Info(), some));");
      const auto begin = source.find("return match sample.data");
      const auto end = source.find("};", begin);
      ASSERT_NE(begin, std::string::npos);
      ASSERT_NE(end, std::string::npos);
      source.replace(begin, end + 2 - begin,
                     "var value = sample.data.take(0); return value.v - 7;");
    }
    write("nested.sun", source);
    ASSERT_NO_FATAL_FAILURE(checkProgram("nested.sun"));

    const auto consumerStart = source.find("class Msg");
    ASSERT_NE(consumerStart, std::string::npos);
    ASSERT_NO_FATAL_FAILURE(
        buildLibrary(source.substr(0, consumerStart) +
                     "manifest { libraries: [\"stdlib.moon\"] }"));

    std::string app = "using std;\n" + source.substr(consumerStart);
    const auto manifest = app.find("manifest");
    ASSERT_NE(manifest, std::string::npos);
    app.replace(manifest, app.size() - manifest,
                "manifest { libraries: [\"stdlib.moon\", \"lib.moon\"] }");
    write("consumer.sun", app);
    ASSERT_NO_FATAL_FAILURE(checkProgram("consumer.sun"));
  }
}

}  // namespace
