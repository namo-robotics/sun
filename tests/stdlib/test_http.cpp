// tests/stdlib/test_http.cpp - Tests for stdlib http.sun
//
// Exercises request parsing and response building without sockets;
// examples/80-http_server/test.sh covers the end-to-end path.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// Builds "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n"
static const char* kBuildRequest = R"(
    function build_request(alloc: ref HeapAllocator) String {
        var raw = String(alloc, "GET /index.html HTTP/1.1");
        append_crlf(raw);
        raw.append("Host: x");
        append_crlf(raw);
        append_crlf(raw);
        return raw;
    }
)";

TEST(Stdlib_Http, parse_request_method_and_path) {
  std::string source = std::string("using sun;\n") + kBuildRequest + R"(
    function main() i64 {
        var alloc = make_heap_allocator();
        var raw = build_request(alloc);
        var req = HttpRequest(alloc);
        parse_http_request(raw, req);
        if (req.method.equals_literal("GET") == false) { return 1; }
        if (req.path.equals_literal("/index.html") == false) { return 2; }
        if (req.headers.starts_with("Host: x") == false) { return 3; }
        return 0;
    }
  )";
  EXPECT_EQ(executeStringWithStdlib(source), 0);
}

TEST(Stdlib_Http, find_header_end) {
  std::string source = std::string("using sun;\n") + kBuildRequest + R"(
    function main() i64 {
        var alloc = make_heap_allocator();
        var complete = build_request(alloc);
        var incomplete = String(alloc, "GET / HTTP/1.1");
        append_crlf(incomplete);
        if (find_header_end(incomplete) >= 0) { return 1; }
        if (find_header_end(complete) < 0) { return 2; }
        return 0;
    }
  )";
  EXPECT_EQ(executeStringWithStdlib(source), 0);
}

// The terminator is CRLFCRLF, so the lone CRLFs that end each header line
// must not match, and the index returned is the start of the run.
TEST(Stdlib_Http, find_header_end_returns_the_run_start) {
  std::string source = R"(
    using sun;

    function main() i64 {
        var alloc = make_heap_allocator();
        var s = String(alloc, "GET / HTTP/1.1\r\nHost: x\r\n\r\nbody");
        // The run starts at the CRLF that ends the last header line, not
        // after it: "GET / HTTP/1.1" (14) + CRLF (2) + "Host: x" (7) = 23.
        if (find_header_end(s) != 23) { return 1; }

        // A terminator at the very start, and one at the very end
        var lead = String(alloc, "\r\n\r\nrest");
        if (find_header_end(lead) != 0) { return 2; }
        var trail = String(alloc, "ab\r\n\r\n");
        if (find_header_end(trail) != 2) { return 3; }

        // Three CRs in a row: the match starts at the first complete run
        var tricky = String(alloc, "\r\r\n\r\n");
        if (find_header_end(tricky) != 1) { return 4; }

        // Never a partial match
        var cut = String(alloc, "a\r\n\r");
        if (find_header_end(cut) >= 0) { return 5; }
        var empty = String(alloc, "");
        if (find_header_end(empty) >= 0) { return 6; }
        return 0;
    }
  )";
  EXPECT_EQ(executeStringWithStdlib(source), 0);
}

TEST(Stdlib_Http, response_defaults_and_setters) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        var alloc = make_heap_allocator();
        var resp = HttpResponse(alloc);
        if (resp.status != 200) { return 1; }
        if (resp.content_type.equals_literal("text/html") == false) { return 2; }
        if (resp.body.length() != 0) { return 3; }

        resp.set_status(404);
        resp.set_content_type("application/json");
        resp.set_body("{}");
        if (resp.status != 404) { return 4; }
        if (resp.content_type.equals_literal("application/json") == false) { return 5; }
        if (resp.body.equals_literal("{}") == false) { return 6; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Http, status_reason_phrases) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        var alloc = make_heap_allocator();
        var ok = String(alloc, "");
        append_reason_phrase(ok, 200);
        if (ok.equals_literal("OK") == false) { return 1; }

        var nf = String(alloc, "");
        append_reason_phrase(nf, 404);
        if (nf.equals_literal("Not Found") == false) { return 2; }

        var unknown = String(alloc, "");
        append_reason_phrase(unknown, 599);
        if (unknown.equals_literal("Status") == false) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Http, server_handler_lambda_compiles) {
  // Full serve() loop needs a live socket; this verifies the lambda-based
  // API wires up (bind to port 0 is skipped - just construct and stop).
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i64 {
        var alloc = make_heap_allocator();
        var server = HttpServer(alloc);
        var req = HttpRequest(alloc);
        var resp = HttpResponse(alloc);
        var handler = lambda (r: ref HttpRequest, w: ref HttpResponse) void {
            w.set_status(404);
        };
        handler(req, resp);
        server.stop();
        return resp.status;
    }
  )");
  EXPECT_EQ(value, 404);
}
