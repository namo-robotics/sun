// tests/stdlib/test_tls.cpp - Tests for the tls.moon bundle
//
// Response parsing runs without a network. The connection tests need a TLS
// peer, so they generate a throwaway CA and certificate and run OpenSSL's
// own s_server against it; they skip when tls.moon or the openssl tool is
// missing rather than failing a build without TLS support.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "driver/execution_utils.h"

namespace {

// The tls bundle plus the stdlib it depends on, or empty if tls.moon has not
// been built (scripts/build-openssl.sh supplies what it needs).
std::vector<sun::MoonImport> tlsMoonImports() {
  auto imports = getStdlibMoonImports();
  if (imports.empty()) return {};

  auto tlsPath =
      std::filesystem::path(imports.front().path).parent_path() / "tls.moon";
  if (!std::filesystem::exists(tlsPath)) return {};

  imports.push_back({tlsPath.string(), {}});
  return imports;
}

sun::SunValue executeWithTls(const std::string& source) {
  initTestEnvironment();
  auto driver = Driver::createForJIT();
  driver->setMoonImports(tlsMoonImports());
  return driver->executeString(source);
}

bool haveOpenSSLTool() { return std::system("which openssl > /dev/null 2>&1") == 0; }

// A self-signed certificate plus an s_server serving it, torn down with the
// fixture. Trusting it means pointing SSL_CERT_FILE at the same PEM.
class TlsServer {
 public:
  TlsServer(const std::string& commonName, int port) : port_(port) {
    dir_ = std::filesystem::temp_directory_path() /
           ("sun-tls-test-" + std::to_string(::getpid()) + "-" +
            std::to_string(port));
    std::filesystem::create_directories(dir_);
    certPath_ = (dir_ / "cert.pem").string();
    std::string keyPath = (dir_ / "key.pem").string();

    std::string gen = "openssl req -x509 -newkey rsa:2048 -keyout " + keyPath +
                      " -out " + certPath_ +
                      " -days 1 -nodes -subj '/CN=" + commonName +
                      "' -addext 'subjectAltName=DNS:" + commonName +
                      "' > /dev/null 2>&1";
    ok_ = std::system(gen.c_str()) == 0;
    if (!ok_) return;

    std::string serve = "openssl s_server -accept " + std::to_string(port) +
                        " -www -cert " + certPath_ + " -key " + keyPath +
                        " > /dev/null 2>&1 & echo $! > " +
                        (dir_ / "pid").string();
    ok_ = std::system(serve.c_str()) == 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
  }

  ~TlsServer() {
    std::system(("kill $(cat " + (dir_ / "pid").string() + ") 2>/dev/null")
                    .c_str());
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  bool ok() const { return ok_; }
  const std::string& certPath() const { return certPath_; }
  int port() const { return port_; }

 private:
  std::filesystem::path dir_;
  std::string certPath_;
  int port_ = 0;
  bool ok_ = false;
};

// Fetch "/" over TLS from a local server; returns the status code, or a
// negative marker when connecting failed.
std::string fetchProgram(int port) {
  return R"(
    using sun;
    using tls;

    function main() i64 {
        var alloc = make_heap_allocator();
        var client = HttpsClient(alloc);
        var host = String(alloc, "localhost");
        try {
            var resp = client.get(host, )" +
         std::to_string(port) + R"(, "/");
            return _convert<i64>(resp.status);
        } catch (e: IError) {
            return -1;
        }
    }
  )";
}

}  // namespace

// ---------------------------------------------------------------------------
// Response parsing (no network)
// ---------------------------------------------------------------------------

TEST(Stdlib_Tls, parses_status_headers_and_body) {
  if (tlsMoonImports().empty()) GTEST_SKIP() << "tls.moon not built";
  auto value = executeWithTls(R"(
    using sun;
    using tls;

    function main() i64 {
        var alloc = make_heap_allocator();
        var raw = String(alloc, "HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\n\r\nhello");
        var resp = HttpClientResponse(alloc);
        try {
            parse_response(alloc, raw, resp);
        } catch (e: IError) {
            return 90;
        }
        if (resp.status != 404) { return 1; }
        if (resp.body.equals_literal("hello") == false) { return 2; }
        if (resp.headers.starts_with("Content-Length: 5") == false) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Tls, decodes_chunked_body) {
  if (tlsMoonImports().empty()) GTEST_SKIP() << "tls.moon not built";
  auto value = executeWithTls(R"(
    using sun;
    using tls;

    function main() i64 {
        var alloc = make_heap_allocator();
        var raw = String(alloc, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
        var resp = HttpClientResponse(alloc);
        try {
            parse_response(alloc, raw, resp);
        } catch (e: IError) {
            return 90;
        }
        if (resp.status != 200) { return 1; }
        if (resp.body.equals_literal("hello world") == false) { return 2; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Tls, rejects_response_without_header_terminator) {
  if (tlsMoonImports().empty()) GTEST_SKIP() << "tls.moon not built";
  auto value = executeWithTls(R"(
    using sun;
    using tls;

    function main() i64 {
        var alloc = make_heap_allocator();
        var raw = String(alloc, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n");
        var resp = HttpClientResponse(alloc);
        try {
            parse_response(alloc, raw, resp);
        } catch (e: IError) {
            return 0;
        }
        return 1;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ---------------------------------------------------------------------------
// DNS (stdlib, no network beyond the local resolver)
// ---------------------------------------------------------------------------

TEST(Stdlib_Networking, resolves_localhost_to_loopback) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    function main() i64 {
        var alloc = make_heap_allocator();
        var host = String(alloc, "localhost");
        try {
            // 127.0.0.1 in network byte order
            if (resolve_host_ipv4(host) != 16777343) { return 1; }
            return 0;
        } catch (e: IError) {
            return 2;
        }
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Networking, unresolvable_host_throws) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    function main() i64 {
        var alloc = make_heap_allocator();
        var host = String(alloc, "no-such-host.invalid");
        try {
            resolve_host_ipv4(host);
            return 1;
        } catch (e: IError) {
            return 0;
        }
    }
  )");
  EXPECT_EQ(value, 0);
}

// ---------------------------------------------------------------------------
// Handshake and certificate verification (local TLS server)
// ---------------------------------------------------------------------------

TEST(Stdlib_Tls, connects_to_server_with_trusted_certificate) {
  if (tlsMoonImports().empty()) GTEST_SKIP() << "tls.moon not built";
  if (!haveOpenSSLTool()) GTEST_SKIP() << "openssl tool not available";

  TlsServer server("localhost", 18443);
  ASSERT_TRUE(server.ok()) << "could not start local TLS server";

  // Trusting the throwaway CA is what makes verification succeed
  setenv("SSL_CERT_FILE", server.certPath().c_str(), 1);
  auto value = executeWithTls(fetchProgram(server.port()));
  unsetenv("SSL_CERT_FILE");

  EXPECT_EQ(value, 200);
}

TEST(Stdlib_Tls, rejects_untrusted_certificate_by_default) {
  if (tlsMoonImports().empty()) GTEST_SKIP() << "tls.moon not built";
  if (!haveOpenSSLTool()) GTEST_SKIP() << "openssl tool not available";

  TlsServer server("localhost", 18444);
  ASSERT_TRUE(server.ok()) << "could not start local TLS server";

  // No SSL_CERT_FILE: the self-signed certificate chains to nothing trusted
  unsetenv("SSL_CERT_FILE");
  EXPECT_EQ(executeWithTls(fetchProgram(server.port())), -1);
}

TEST(Stdlib_Tls, rejects_certificate_for_a_different_hostname) {
  if (tlsMoonImports().empty()) GTEST_SKIP() << "tls.moon not built";
  if (!haveOpenSSLTool()) GTEST_SKIP() << "openssl tool not available";

  // Certificate is valid and trusted, but issued for another name
  TlsServer server("otherhost.example", 18445);
  ASSERT_TRUE(server.ok()) << "could not start local TLS server";

  setenv("SSL_CERT_FILE", server.certPath().c_str(), 1);
  auto value = executeWithTls(fetchProgram(server.port()));
  unsetenv("SSL_CERT_FILE");

  EXPECT_EQ(value, -1);
}
