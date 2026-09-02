// tests/stdlib/test_json.cpp - The compile-time ownership rule for Json: a
// String passed to Json(s) is moved in, so using it afterwards is rejected.
// The runtime behavior tests live in stdlib/json_tests.sun.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Stdlib_Json, json_constructor_takes_ownership_of_string) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;

    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var s = String(alloc, "moved");
        var req = create_json_object(alloc);
        req.set(String(alloc, "s"), Json(s));
        // s was moved into the document; this is a use after move
        if (s.length() == 0) { return 1; }
        return 0;
    }
  )"),
               std::exception);
}
