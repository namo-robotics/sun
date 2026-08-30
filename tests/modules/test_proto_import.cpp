// tests/modules/test_proto_import.cpp - Native protobuf import: the `protos:`
// manifest key, manifest resolution, and (later milestones) the synthesized
// message classes with encode/decode.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "ast.h"
#include "driver/execution_utils.h"
#include "driver/manifest_processor.h"
#include "parsing/parser.h"
#include "proto_test_utils.h"
#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"

namespace fs = std::filesystem;
using proto_test::LibprotobufSchema;
using proto_test::ProtoProject;
using proto_test::runWithProto;

namespace {

// Parse a program and return its manifest block (nullptr if none)
const ManifestAST* parseManifest(const std::string& source,
                                 std::unique_ptr<BlockExprAST>& keepAlive) {
  auto parser = Parser::createStringParser(source);
  keepAlive = parser.parseProgram();
  return sun::ManifestProcessor::findManifest(*keepAlive);
}

}  // namespace

// ============================================================================
// Manifest grammar
// ============================================================================

TEST(Modules_ProtoImport, manifest_protos_key_parses) {
  std::unique_ptr<BlockExprAST> ast;
  const auto* manifest = parseManifest(R"(
    manifest {
      protos: ["schemas/telemetry.proto", "schemas/control.proto"]
    }
    function main() i32 { return 0; }
  )",
                                       ast);
  ASSERT_NE(manifest, nullptr);
  ASSERT_EQ(manifest->getProtos().size(), 2u);
  EXPECT_EQ(manifest->getProtos()[0].path, "schemas/telemetry.proto");
  EXPECT_EQ(manifest->getProtos()[1].path, "schemas/control.proto");
  EXPECT_TRUE(manifest->getSuns().empty());
  EXPECT_TRUE(manifest->getMoons().empty());
}

TEST(Modules_ProtoImport, manifest_entries_tolerate_trailing_semicolons) {
  std::unique_ptr<BlockExprAST> ast;
  const auto* manifest = parseManifest(R"(
    manifest {
      protos: ["a.proto"];
      libraries: ["stdlib.moon"];
      source_files: ["helper.sun"];
    }
    function main() i32 { return 0; }
  )",
                                       ast);
  ASSERT_NE(manifest, nullptr);
  EXPECT_EQ(manifest->getProtos().size(), 1u);
  EXPECT_EQ(manifest->getMoons().size(), 1u);
  EXPECT_EQ(manifest->getSuns().size(), 1u);
}

TEST(Modules_ProtoImport, manifest_protos_mixed_with_other_keys) {
  std::unique_ptr<BlockExprAST> ast;
  const auto* manifest = parseManifest(R"(
    manifest {
      source_files: ["helper.sun"]
      protos: ["a.proto"]
      libraries: ["stdlib.moon"]
    }
    function main() i32 { return 0; }
  )",
                                       ast);
  ASSERT_NE(manifest, nullptr);
  EXPECT_EQ(manifest->getSuns().size(), 1u);
  EXPECT_EQ(manifest->getProtos().size(), 1u);
  EXPECT_EQ(manifest->getMoons().size(), 1u);
}

TEST(Modules_ProtoImport, manifest_protos_rejects_non_string_entries) {
  EXPECT_THROW(
      {
        std::unique_ptr<BlockExprAST> ast;
        parseManifest(R"(
          manifest { protos: [42] }
          function main() i32 { return 0; }
        )",
                      ast);
      },
      std::exception);
}

TEST(Modules_ProtoImport, manifest_unknown_key_names_protos_in_error) {
  try {
    std::unique_ptr<BlockExprAST> ast;
    parseManifest(R"(
      manifest { schemas: ["a.proto"] }
      function main() i32 { return 0; }
    )",
                  ast);
    FAIL() << "expected a parse error";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("protos"), std::string::npos);
  }
}

TEST(Modules_ProtoImport, manifest_moon_url_parses) {
  std::unique_ptr<BlockExprAST> ast;
  const auto* manifest = parseManifest(R"(
    manifest {
      libraries: [{ url: "https://example.com/libs/mylib.moon", hash: "abc123", rename: "ml" }]
    }
    function main() i32 { return 0; }
  )",
                                       ast);
  ASSERT_NE(manifest, nullptr);
  ASSERT_EQ(manifest->getMoons().size(), 1u);
  const auto& moon = manifest->getMoons()[0];
  EXPECT_TRUE(moon.path.empty());
  ASSERT_TRUE(moon.url.has_value());
  EXPECT_EQ(*moon.url, "https://example.com/libs/mylib.moon");
  ASSERT_TRUE(moon.hash.has_value());
  EXPECT_EQ(*moon.hash, "abc123");
  ASSERT_TRUE(moon.rename.has_value());
  EXPECT_EQ(*moon.rename, "ml");
}

TEST(Modules_ProtoImport, manifest_moon_url_and_path_conflict) {
  EXPECT_THROW(
      {
        std::unique_ptr<BlockExprAST> ast;
        parseManifest(R"(
          manifest {
            libraries: [{ path: "a.moon", url: "https://example.com/a.moon" }]
          }
          function main() i32 { return 0; }
        )",
                      ast);
      },
      std::exception);
}

TEST(Modules_ProtoImport, manifest_moon_requires_path_or_url) {
  EXPECT_THROW(
      {
        std::unique_ptr<BlockExprAST> ast;
        parseManifest(R"(
          manifest { libraries: [{ hash: "abc" }] }
          function main() i32 { return 0; }
        )",
                      ast);
      },
      std::exception);
}

// ============================================================================
// Serialization round-trip
// ============================================================================

TEST(Modules_ProtoImport, manifest_protos_serialization_roundtrip) {
  std::vector<ManifestSunDependency> suns;
  std::vector<ManifestMoonDependency> moons;
  std::vector<ManifestProtoDependency> protos;
  protos.push_back({"schemas/telemetry.proto"});
  protos.push_back({"schemas/control.proto"});
  auto ast = std::make_unique<ManifestAST>(std::move(suns), std::move(moons),
                                           std::move(protos));

  sun::serialization::ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  sun::serialization::ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::MANIFEST);
  const auto& manifest = static_cast<const ManifestAST&>(*restored);
  ASSERT_EQ(manifest.getProtos().size(), 2u);
  EXPECT_EQ(manifest.getProtos()[0].path, "schemas/telemetry.proto");
  EXPECT_EQ(manifest.getProtos()[1].path, "schemas/control.proto");
}

TEST(Modules_ProtoImport, manifest_moon_url_serialization_roundtrip) {
  std::vector<ManifestSunDependency> suns;
  std::vector<ManifestMoonDependency> moons;
  std::vector<ManifestProtoDependency> protos;
  ManifestMoonDependency dep;
  dep.url = "https://example.com/lib.moon";
  dep.hash = "abc123";
  moons.push_back(std::move(dep));
  auto ast = std::make_unique<ManifestAST>(std::move(suns), std::move(moons),
                                           std::move(protos));

  sun::serialization::ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  sun::serialization::ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::MANIFEST);
  const auto& manifest = static_cast<const ManifestAST&>(*restored);
  ASSERT_EQ(manifest.getMoons().size(), 1u);
  const auto& moon = manifest.getMoons()[0];
  EXPECT_TRUE(moon.path.empty());
  ASSERT_TRUE(moon.url.has_value());
  EXPECT_EQ(*moon.url, "https://example.com/lib.moon");
  ASSERT_TRUE(moon.hash.has_value());
  EXPECT_EQ(*moon.hash, "abc123");
  EXPECT_FALSE(moon.rename.has_value());
}

// ============================================================================
// ManifestProcessor resolution
// ============================================================================

TEST(Modules_ProtoImport, manifest_processor_resolves_protos_relative_to_base) {
  fs::path dir = fs::temp_directory_path() / "sun_proto_manifest_test";
  fs::create_directories(dir / "schemas");
  {
    std::ofstream out(dir / "schemas" / "t.proto");
    out << "syntax = \"proto3\";\n";
  }
  {
    std::ofstream out(dir / "main.sun");
    out << "manifest { protos: [\"schemas/t.proto\"] }\n"
           "function main() i32 { return 0; }\n";
  }

  auto resolved =
      sun::ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());
  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->protoFiles.size(), 1u);
  EXPECT_EQ(fs::path(resolved->protoFiles[0]).lexically_normal(),
            (dir / "schemas" / "t.proto").lexically_normal());
  EXPECT_TRUE(resolved->sunFiles.empty());
}

TEST(Modules_ProtoImport, manifest_path_variables_expand_in_all_entry_kinds) {
  fs::path dir = fs::temp_directory_path() / "sun_pathvar_test";
  fs::remove_all(dir);
  fs::create_directories(dir / "libs");
  {
    std::ofstream out(dir / "libs" / "helper.sun");
    out << "public function helper() i32 { return 1; }\n";
  }
  {
    std::ofstream out(dir / "libs" / "t.proto");
    out << "syntax = \"proto3\";\n";
  }
  {
    std::ofstream out(dir / "main.sun");
    out << "manifest {\n"
           "  source_files: [\"$TESTLIBS/helper.sun\"]\n"
           "  libraries: [\"$TESTLIBS/lib.moon\"]\n"
           "  protos: [\"$TESTLIBS/t.proto\"]\n"
           "}\n"
           "function main() i32 { return 0; }\n";
  }

  sun::ManifestProcessor::setPathVariable("TESTLIBS", (dir / "libs").string());
  auto resolved =
      sun::ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());
  sun::ManifestProcessor::clearPathVariables();

  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->sunFiles.size(), 1u);
  EXPECT_EQ(fs::path(resolved->sunFiles[0]).lexically_normal(),
            (dir / "libs" / "helper.sun").lexically_normal());
  ASSERT_EQ(resolved->moonImports.size(), 1u);
  EXPECT_EQ(fs::path(resolved->moonImports[0].path).lexically_normal(),
            (dir / "libs" / "lib.moon").lexically_normal());
  ASSERT_EQ(resolved->protoFiles.size(), 1u);
  EXPECT_EQ(fs::path(resolved->protoFiles[0]).lexically_normal(),
            (dir / "libs" / "t.proto").lexically_normal());
}

TEST(Modules_ProtoImport, manifest_path_variable_falls_back_to_environment) {
  fs::path dir = fs::temp_directory_path() / "sun_pathvar_env_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    std::ofstream out(dir / "main.sun");
    out << "manifest { libraries: [\"$SUN_TEST_ENV_LIBS/lib.moon\"] }\n"
           "function main() i32 { return 0; }\n";
  }

  sun::ManifestProcessor::clearPathVariables();
  setenv("SUN_TEST_ENV_LIBS", "/opt/sunlibs", 1);
  auto resolved =
      sun::ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());
  unsetenv("SUN_TEST_ENV_LIBS");

  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->moonImports.size(), 1u);
  EXPECT_EQ(resolved->moonImports[0].path, "/opt/sunlibs/lib.moon");
}

TEST(Modules_ProtoImport, manifest_undefined_path_variable_is_an_error) {
  fs::path dir = fs::temp_directory_path() / "sun_pathvar_undef_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    std::ofstream out(dir / "main.sun");
    out << "manifest { source_files: [\"$SUN_TEST_NO_SUCH_VAR/x.sun\"] }\n"
           "function main() i32 { return 0; }\n";
  }

  sun::ManifestProcessor::clearPathVariables();
  unsetenv("SUN_TEST_NO_SUCH_VAR");
  try {
    sun::ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());
    FAIL() << "expected an undefined-variable error";
  } catch (const SunError& e) {
    EXPECT_NE(std::string(e.what()).find("SUN_TEST_NO_SUCH_VAR"),
              std::string::npos);
  }
}

TEST(Modules_ProtoImport, manifest_processor_returns_nullopt_without_manifest) {
  fs::path dir = fs::temp_directory_path() / "sun_proto_manifest_test2";
  fs::create_directories(dir);
  {
    std::ofstream out(dir / "plain.sun");
    out << "function main() i32 { return 0; }\n";
  }
  auto resolved =
      sun::ManifestProcessor::fromEntrypointFile((dir / "plain.sun").string());
  EXPECT_FALSE(resolved.has_value());
}

// ============================================================================
// Wire helpers (stdlib/proto_wire.sun)
// ============================================================================

TEST(Modules_ProtoImport, wire_varint_roundtrip_small_and_multibyte) {
  auto value = executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var buf = Vec<u8>(alloc, 16);
      proto_write_varint(buf, 1);
      proto_write_varint(buf, 300);
      proto_write_varint(buf, 0);
      // 300 = 0xAC 0x02 (two bytes)
      if (buf.size() != 4) { return -1; }
      if (buf.get_unchecked(1) != 172) { return -2; }
      if (buf.get_unchecked(2) != 2) { return -3; }
      var r = ProtoReader(buf);
      var a: u64 = 0;
      var b: u64 = 0;
      var c: u64 = 0;
      try {
        a = r.read_varint();
        b = r.read_varint();
        c = r.read_varint();
      } catch (e: IError) {
        return -4;
      }
      if (a != 1) { return -5; }
      if (b != 300) { return -6; }
      if (c != 0) { return -7; }
      if (r.at_end() == false) { return -8; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, wire_negative_int32_is_ten_bytes_and_roundtrips) {
  auto value = executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var buf = Vec<u8>(alloc, 16);
      var neg: i32 = -1;
      proto_write_int32(buf, neg);
      if (buf.size() != 10) { return -1; }
      var r = ProtoReader(buf);
      var back: i32 = 0;
      try { back = r.read_int32(); } catch (e: IError) { return -2; }
      if (back != -1) { return -3; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, wire_zigzag_sint_roundtrip) {
  auto value = executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var buf = Vec<u8>(alloc, 16);
      var m1: i32 = -1;
      var p2: i32 = 2;
      var big: i64 = -123456789012;
      proto_write_sint32(buf, m1);   // zigzag(-1) = 1
      proto_write_sint32(buf, p2);   // zigzag(2)  = 4
      proto_write_sint64(buf, big);
      if (buf.get_unchecked(0) != 1) { return -1; }
      if (buf.get_unchecked(1) != 4) { return -2; }
      var r = ProtoReader(buf);
      var a: i32 = 0;
      var b: i32 = 0;
      var c: i64 = 0;
      try {
        a = r.read_sint32();
        b = r.read_sint32();
        c = r.read_sint64();
      } catch (e: IError) { return -3; }
      if (a != -1) { return -4; }
      if (b != 2) { return -5; }
      if (c != -123456789012) { return -6; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, wire_fixed_and_float_roundtrip) {
  auto value = executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var buf = Vec<u8>(alloc, 32);
      var u: u32 = 305419896;   // 0x12345678
      var d: f64 = 3.5;
      var f: f32 = -2.25;
      var s: i64 = -5;
      proto_write_fixed32(buf, u);
      proto_write_double(buf, d);
      proto_write_float(buf, f);
      proto_write_sfixed64(buf, s);
      if (buf.size() != 4 + 8 + 4 + 8) { return -1; }
      if (buf.get_unchecked(0) != 120) { return -2; }  // little-endian 0x78
      var r = ProtoReader(buf);
      var u2: u32 = 0;
      var d2: f64 = 0.0;
      var f2: f32 = 0.0;
      var s2: i64 = 0;
      try {
        u2 = r.read_fixed32();
        d2 = r.read_double();
        f2 = r.read_float();
        s2 = r.read_sfixed64();
      } catch (e: IError) { return -3; }
      if (u2 != u) { return -4; }
      if (d2 != 3.5) { return -5; }
      if (f2 != -2.25) { return -6; }
      if (s2 != -5) { return -7; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, wire_string_and_bytes_roundtrip) {
  auto value = executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var buf = Vec<u8>(alloc, 32);
      var s = String(alloc, "hello");
      var raw = Vec<u8>(alloc, 4);
      raw.push(7);
      raw.push(9);
      proto_write_string(buf, s);
      proto_write_bytes(buf, raw);
      // 1 + 5 + 1 + 2
      if (buf.size() != 9) { return -1; }
      var r = ProtoReader(buf);
      var back = String(alloc, "");
      var backBytes = Vec<u8>(alloc, 1);
      try {
        back = r.read_string_field(alloc);
        backBytes = r.read_bytes_field(alloc);
      } catch (e: IError) { return -2; }
      if (back.length() != 5) { return -3; }
      if (back.at(4) != 111) { return -4; }   // 'o'
      if (backBytes.size() != 2) { return -5; }
      if (backBytes.get_unchecked(1) != 9) { return -6; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, wire_tags_limits_and_skip_unknown) {
  auto value = executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var buf = Vec<u8>(alloc, 32);
      // field 3, varint 150 ; field 4, len-delimited "ab" ; field 5 fixed32 1
      proto_write_tag(buf, 3, 0);
      proto_write_varint(buf, 150);
      proto_write_tag(buf, 4, 2);
      var ab = String(alloc, "ab");
      proto_write_string(buf, ab);
      proto_write_tag(buf, 5, 5);
      var one: u32 = 1;
      proto_write_fixed32(buf, one);

      var r = ProtoReader(buf);
      var unknown = Vec<u8>(alloc, 32);
      var seen3: u64 = 0;
      try {
        var t1: u64 = r.read_tag();
        if (proto_read_tag_field(t1) != 3) { return -1; }
        if (proto_read_tag_wire_type(t1) != 0) { return -2; }
        seen3 = r.read_varint();
        var t2: u64 = r.read_tag();
        r.skip_field(proto_read_tag_wire_type(t2), t2, unknown);
        var t3: u64 = r.read_tag();
        r.skip_field(proto_read_tag_wire_type(t3), t3, unknown);
      } catch (e: IError) { return -3; }
      if (seen3 != 150) { return -4; }
      if (r.at_end() == false) { return -5; }
      // unknown holds: tag(4,2)=34, len 2, 'a','b', tag(5,5)=45, 4 bytes
      if (unknown.size() != 1 + 1 + 2 + 1 + 4) { return -6; }
      if (unknown.get_unchecked(0) != 34) { return -7; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, wire_truncated_input_throws) {
  auto value = executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var buf = Vec<u8>(alloc, 4);
      buf.push(128);   // continuation bit set, then EOF
      var r = ProtoReader(buf);
      try {
        var v: u64 = r.read_varint();
        return -1;
      } catch (e: IError) {
        return 1;
      }
      return -2;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Modules_ProtoImport, map_string_keys) {
  auto value = executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var m = Map<String, i32>(alloc, 8);
      m.insert(String(alloc, "one"), 1);
      m.insert(String(alloc, "two"), 2);
      m.insert(String(alloc, "one"), 11);   // overwrite
      var got: i32 = 0;
      try {
        got = m.get(String(alloc, "one")) + m.get(String(alloc, "two"));
      } catch (e: IError) { return -1; }
      if (m.contains(String(alloc, "three"))) { return -2; }
      if (m.size() != 2) { return -3; }
      return got;
    }
  )");
  EXPECT_EQ(value, 13);
}

// ============================================================================
// Synthesized messages: encode/decode round-trips
// ============================================================================

namespace {

const char* kTelemetryProto = R"(
syntax = "proto3";
package t;
enum Mode { IDLE = 0; ACTIVE = 1; FAULT = 7; }
message Pose { double x = 1; double y = 2; }
message Status {
  int32 robot_id = 1;
  string name = 2;
  repeated int64 samples = 3;
  Mode mode = 4;
  Pose pose = 5;
  bytes blob = 6;
  repeated string tags = 7;
  bool ok = 8;
  sint32 delta = 9;
  fixed32 crc = 10;
  uint64 big = 11;
  float ratio = 12;
}
)";

}  // namespace

TEST(Modules_ProtoImport, message_roundtrip_all_field_kinds) {
  auto value = runWithProto("sun_proto_rt1", kTelemetryProto, R"(
    using std;
    using t;
    function main() i32 {
      var alloc = make_heap_allocator();
      var st = Status(alloc);
      st.robot_id = 7;
      st.name = String(alloc, "rover");
      st.samples.push(1);
      st.samples.push(300);
      st.samples.push(-5);
      st.mode = Mode.FAULT;
      st.pose.x = 1.5;
      st.pose.y = -2.0;
      st.blob.push(9);
      st.blob.push(0);
      st.tags.push(String(alloc, "a"));
      st.tags.push(String(alloc, "bc"));
      st.ok = true;
      st.delta = -3;
      st.crc = 305419896;
      st.big = 1234567890123;
      st.ratio = 0.5;

      var buf = Vec<u8>(alloc, 64);
      st.encode(buf);
      try {
        var b = Status_decode(alloc, buf);
        if (b.robot_id != 7) { return 1; }
        if (b.name.length() != 5) { return 2; }
        if (b.samples.size() != 3) { return 3; }
        if (b.samples.get_unchecked(1) != 300) { return 4; }
        if (b.samples.get_unchecked(2) != -5) { return 5; }
        if (proto_enum_to_i32_Mode(b.mode) != 7) { return 6; }
        if (b.pose.x != 1.5) { return 7; }
        if (b.pose.y != -2.0) { return 8; }
        if (b.blob.size() != 2) { return 9; }
        if (b.tags.size() != 2) { return 10; }
        if (b.tags.get_unchecked(1).length() != 2) { return 11; }
        if (b.ok == false) { return 12; }
        if (b.delta != -3) { return 13; }
        if (b.crc != 305419896) { return 14; }
        if (b.big != 1234567890123) { return 15; }
        if (b.ratio != 0.5) { return 16; }
      } catch (e: IError) {
        return -1;
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, zero_values_encode_to_empty_and_decode_defaults) {
  auto value = runWithProto("sun_proto_rt2", kTelemetryProto, R"(
    using std;
    using t;
    function main() i32 {
      var alloc = make_heap_allocator();
      var st = Status(alloc);
      var buf = Vec<u8>(alloc, 8);
      st.encode(buf);
      // Only the always-written empty sub-message: tag(5,2) + len 0
      if (buf.size() != 2) { return 1; }
      var empty = Vec<u8>(alloc, 1);
      try {
        var b = Status_decode(alloc, empty);
        if (b.robot_id != 0) { return 2; }
        if (b.name.length() != 0) { return 3; }
        if (b.samples.size() != 0) { return 4; }
        if (proto_enum_to_i32_Mode(b.mode) != 0) { return 5; }
        if (b.ok) { return 6; }
      } catch (e: IError) { return -1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, unknown_fields_survive_reencode) {
  auto value = runWithProto("sun_proto_rt3", kTelemetryProto, R"(
    using std;
    using t;
    function main() i32 {
      var alloc = make_heap_allocator();
      // Hand-built: field 1 = 9, then unknown field 99 (varint 5), then
      // unknown field 100 (len-delimited "xy")
      var wire = Vec<u8>(alloc, 16);
      proto_write_tag(wire, 1, 0);
      proto_write_int32(wire, 9);
      proto_write_tag(wire, 99, 0);
      proto_write_varint(wire, 5);
      proto_write_tag(wire, 100, 2);
      var xy = String(alloc, "xy");
      proto_write_string(wire, xy);
      var n: i64 = wire.size();
      try {
        var m = Status_decode(alloc, wire);
        if (m.robot_id != 9) { return 1; }
        var out = Vec<u8>(alloc, 16);
        m.encode(out);
        // robot_id + empty pose (2 bytes) + unknown bytes replayed
        if (out.size() != n + 2) { return 2; }
        // Decode again: still parses, unknowns still carried
        var m2 = Status_decode(alloc, out);
        if (m2.robot_id != 9) { return 3; }
        if (m2.unknown_fields.size() == 0) { return 4; }
      } catch (e: IError) { return -1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, unpacked_repeated_scalars_decode) {
  auto value = runWithProto("sun_proto_rt4", kTelemetryProto, R"(
    using std;
    using t;
    function main() i32 {
      var alloc = make_heap_allocator();
      // samples (field 3) written unpacked: three separate varint records
      var wire = Vec<u8>(alloc, 16);
      proto_write_tag(wire, 3, 0);
      proto_write_int64(wire, 10);
      proto_write_tag(wire, 3, 0);
      proto_write_int64(wire, 20);
      proto_write_tag(wire, 3, 0);
      proto_write_int64(wire, 30);
      try {
        var m = Status_decode(alloc, wire);
        if (m.samples.size() != 3) { return 1; }
        if (m.samples.get_unchecked(2) != 30) { return 2; }
      } catch (e: IError) { return -1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, unknown_enum_value_maps_to_zero_variant) {
  auto value = runWithProto("sun_proto_rt5", kTelemetryProto, R"(
    using std;
    using t;
    function main() i32 {
      var alloc = make_heap_allocator();
      var wire = Vec<u8>(alloc, 8);
      proto_write_tag(wire, 4, 0);
      proto_write_int32(wire, 42);   // not a declared Mode value
      try {
        var m = Status_decode(alloc, wire);
        return proto_enum_to_i32_Mode(m.mode);
      } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, truncated_message_throws_decode_error) {
  auto value = runWithProto("sun_proto_rt6", kTelemetryProto, R"(
    using std;
    using t;
    function main() i32 {
      var alloc = make_heap_allocator();
      var wire = Vec<u8>(alloc, 8);
      proto_write_tag(wire, 2, 0 + 2);
      proto_write_varint(wire, 50);   // claims 50 bytes, provides none
      try {
        var m = Status_decode(alloc, wire);
        return -1;
      } catch (e: IError) {
        return 1;
      }
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Modules_ProtoImport, nested_message_types_flatten_with_underscore) {
  auto value = runWithProto("sun_proto_rt7", R"(
    syntax = "proto3";
    package n;
    message Outer {
      message Inner { int32 v = 1; }
      enum Kind { A = 0; B = 1; }
      Inner inner = 1;
      Kind kind = 2;
      repeated Inner more = 3;
    }
  )",
                            R"(
    using std;
    using n;
    function main() i32 {
      var alloc = make_heap_allocator();
      var o = Outer(alloc);
      o.inner.v = 5;
      o.kind = Outer_Kind.B;
      var extra = Outer_Inner(alloc);
      extra.v = 6;
      o.more.push(extra);
      var buf = Vec<u8>(alloc, 32);
      o.encode(buf);
      try {
        var b = Outer_decode(alloc, buf);
        if (b.inner.v != 5) { return 1; }
        if (proto_enum_to_i32_Outer_Kind(b.kind) != 1) { return 2; }
        if (b.more.size() != 1) { return 3; }
        if (b.more.get_unchecked(0).v != 6) { return 4; }
      } catch (e: IError) { return -1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, delimited_stream_framing) {
  auto value = runWithProto("sun_proto_rt8", kTelemetryProto, R"(
    using std;
    using t;
    function main() i32 {
      var alloc = make_heap_allocator();
      var a = Pose(alloc);
      a.x = 1.0;
      var b = Pose(alloc);
      b.y = 2.0;
      var stream = Vec<u8>(alloc, 32);
      a.encode_delimited(stream);
      b.encode_delimited(stream);
      try {
        var r = ProtoReader(stream);
        var a2 = Pose_decode_delimited(alloc, r);
        var b2 = Pose_decode_delimited(alloc, r);
        if (a2.x != 1.0) { return 1; }
        if (a2.y != 0.0) { return 2; }
        if (b2.y != 2.0) { return 3; }
        if (r.at_end() == false) { return 4; }
      } catch (e: IError) { return -1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Rejections and diagnostics
// ============================================================================

TEST(Modules_ProtoImport, proto2_syntax_is_rejected) {
  EXPECT_THROW(runWithProto("sun_proto_rej1", R"(
    syntax = "proto2";
    package p;
    message M { optional int32 a = 1; }
  )",
                            "function main() i32 { return 0; }"),
               std::exception);
}

TEST(Modules_ProtoImport, recursive_message_is_rejected) {
  try {
    runWithProto("sun_proto_rej2", R"(
      syntax = "proto3";
      package p;
      message Node { Node child = 1; }
    )",
                 "function main() i32 { return 0; }");
    FAIL() << "expected rejection";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("recursive"), std::string::npos);
  }
}

TEST(Modules_ProtoImport, proto_syntax_error_reports_file_line_column) {
  try {
    runWithProto("sun_proto_rej3", R"(
      syntax = "proto3";
      package p;
      message M { int32 a = ; }
    )",
                 "function main() i32 { return 0; }");
    FAIL() << "expected rejection";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("t.proto:4:"), std::string::npos) << msg;
  }
}

TEST(Modules_ProtoImport, missing_proto_file_is_reported) {
  ProtoProject project("sun_proto_rej4");
  project.setProgram("function main() i32 { return 0; }\n", {"nope.proto"});
  try {
    project.run();
    FAIL() << "expected rejection";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("nope.proto"), std::string::npos);
  }
}

// ============================================================================
// Cross-validation against libprotobuf (linked into the test binary)
// ============================================================================

namespace {

// Encode a Status with fixed values in Sun and return the wire bytes
std::string sunEncodedStatusBytes(ProtoProject& project) {
  fs::path outFile = project.file("bytes.bin");
  project.setProgram(
      "using std;\nusing t;\n"
      "function main() i32 {\n"
      "  var alloc = make_heap_allocator();\n"
      "  var st = Status(alloc);\n"
      "  st.robot_id = 7;\n"
      "  st.name = String(alloc, \"rover\");\n"
      "  st.samples.push(1);\n  st.samples.push(300);\n"
      "  st.mode = Mode.FAULT;\n"
      "  st.pose.x = 1.5;\n  st.pose.y = -2.0;\n"
      "  st.tags.push(String(alloc, \"a\"));\n"
      "  st.ok = true;\n  st.delta = -3;\n  st.crc = 305419896;\n"
      "  var buf = Vec<u8>(alloc, 64);\n"
      "  st.encode(buf);\n" +
      proto_test::dumpBufferProgramTail(outFile));
  EXPECT_EQ(project.run(), 0);
  return proto_test::readBytes(outFile);
}

}  // namespace

TEST(Modules_ProtoImport, libprotobuf_parses_sun_encoded_message) {
  ProtoProject project("sun_proto_xv");
  project.addSchema("t.proto", kTelemetryProto);
  std::string bytes = sunEncodedStatusBytes(project);
  ASSERT_FALSE(bytes.empty());

  namespace pb = google::protobuf;
  LibprotobufSchema schema(project.schemasDir());
  auto msg = schema.parse("t.proto", "Status", bytes);
  ASSERT_NE(msg, nullptr) << schema.errors();
  const pb::Descriptor* desc = msg->GetDescriptor();
  const pb::Reflection* refl = msg->GetReflection();
  EXPECT_EQ(refl->GetInt32(*msg, desc->FindFieldByName("robot_id")), 7);
  EXPECT_EQ(refl->GetString(*msg, desc->FindFieldByName("name")), "rover");
  EXPECT_EQ(refl->FieldSize(*msg, desc->FindFieldByName("samples")), 2);
  EXPECT_EQ(refl->GetRepeatedInt64(*msg, desc->FindFieldByName("samples"), 1),
            300);
  EXPECT_EQ(refl->GetEnumValue(*msg, desc->FindFieldByName("mode")), 7);
  EXPECT_EQ(refl->GetInt32(*msg, desc->FindFieldByName("delta")), -3);
  EXPECT_EQ(refl->GetUInt32(*msg, desc->FindFieldByName("crc")), 305419896u);
  EXPECT_TRUE(refl->GetBool(*msg, desc->FindFieldByName("ok")));
  const pb::Message& pose =
      refl->GetMessage(*msg, desc->FindFieldByName("pose"));
  EXPECT_DOUBLE_EQ(pose.GetReflection()->GetDouble(
                       pose, pose.GetDescriptor()->FindFieldByName("y")),
                   -2.0);
  // Sun-encoded bytes are also byte-identical to libprotobuf's canonical
  // serialization of the same values
  EXPECT_EQ(msg->SerializeAsString(), bytes);
}

// ============================================================================
// Full fidelity: optional, oneof, map, proto imports
// ============================================================================

namespace {
const char* kFullProto = R"(
syntax = "proto3";
package f;
message Item { int32 id = 1; string label = 2; }
message Bag {
  optional string nickname = 1;
  optional int32 count = 2;
  oneof power { int32 battery_pct = 3; string station_id = 4; Item dock = 5; }
  map<string, int32> scores = 6;
  map<int64, Item> items = 7;
  string name = 8;
}
)";
}  // namespace

TEST(Modules_ProtoImport, optional_fields_track_presence) {
  auto value = runWithProto("sun_proto_full1", kFullProto, R"(
    using std;
    using f;
    function main() i32 {
      var alloc = make_heap_allocator();
      var b = Bag(alloc);
      var buf0 = Vec<u8>(alloc, 8);
      b.encode(buf0);
      if (buf0.size() != 0) { return 1; }          // unset optionals: nothing written
      b.nickname = Option.Some(String(alloc, "nick"));
      b.count = Option.Some(0);                     // explicit zero IS written
      var buf = Vec<u8>(alloc, 16);
      b.encode(buf);
      if (buf.size() != 6 + 2) { return 2; }
      try {
        var back = Bag_decode(alloc, buf);
        var nick: i64 = match back.nickname { Option.Some(s) => s.length(), Option.None => -1 };
        if (nick != 4) { return 3; }
        var cnt: i32 = match back.count { Option.Some(v) => 100 + v, Option.None => -1 };
        if (cnt != 100) { return 4; }
        // Decoding an empty buffer leaves optionals unset
        var empty = Vec<u8>(alloc, 1);
        var b2 = Bag_decode(alloc, empty);
        var unset: i32 = match b2.count { Option.Some(v) => 1, Option.None => 0 };
        if (unset != 0) { return 5; }
      } catch (e: IError) { return -1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, oneof_roundtrips_each_variant) {
  auto value = runWithProto("sun_proto_full2", kFullProto, R"(
    using std;
    using f;
    function roundtrip(alloc: ref HeapAllocator, b: ref Bag) i32 throws IError {
      var buf = Vec<u8>(alloc, 32);
      b.encode(buf);
      var back = Bag_decode(alloc, buf);
      return match back.power {
        Bag_power.BatteryPct(v) => 1000 + v,
        Bag_power.StationId(s) => 2000 + _convert<i32>(s.length()),
        Bag_power.Dock(d) => 3000 + d.id,
        Bag_power.NotSet => 0
      };
    }
    function main() i32 {
      var alloc = make_heap_allocator();
      var b = Bag(alloc);
      try {
        if (roundtrip(alloc, b) != 0) { return 1; }
        b.power = Bag_power.BatteryPct(55);
        if (roundtrip(alloc, b) != 1055) { return 2; }
        b.power = Bag_power.StationId(String(alloc, "dock-7"));
        if (roundtrip(alloc, b) != 2006) { return 3; }
        var d = Item(alloc);
        d.id = 9;
        d.label = String(alloc, "nine");
        b.power = Bag_power.Dock(d);
        if (roundtrip(alloc, b) != 3009) { return 4; }
      } catch (e: IError) { return -1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, oneof_last_field_on_wire_wins) {
  auto value = runWithProto("sun_proto_full3", kFullProto, R"(
    using std;
    using f;
    function main() i32 {
      var alloc = make_heap_allocator();
      // Two members of the same oneof on the wire: protobuf keeps the last
      var wire = Vec<u8>(alloc, 16);
      proto_write_tag(wire, 3, 0);
      proto_write_int32(wire, 1);
      proto_write_tag(wire, 4, 2);
      var s = String(alloc, "st");
      proto_write_string(wire, s);
      try {
        var b = Bag_decode(alloc, wire);
        return match b.power {
          Bag_power.StationId(x) => _convert<i32>(x.length()),
          Bag_power.BatteryPct(v) => -2,
          Bag_power.Dock(d) => -3,
          Bag_power.NotSet => -4
        };
      } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Modules_ProtoImport, maps_with_string_keys_and_message_values) {
  auto value = runWithProto("sun_proto_full4", kFullProto, R"(
    using std;
    using f;
    function main() i32 {
      var alloc = make_heap_allocator();
      var b = Bag(alloc);
      b.scores.insert(String(alloc, "alice"), 10);
      b.scores.insert(String(alloc, "bob"), 20);
      var it = Item(alloc);
      it.id = 3;
      it.label = String(alloc, "three");
      b.items.insert(3, it);
      var buf = Vec<u8>(alloc, 64);
      b.encode(buf);
      try {
        var back = Bag_decode(alloc, buf);
        if (back.scores.size() != 2) { return 1; }
        if (back.scores.get(String(alloc, "bob")) != 20) { return 2; }
        if (back.scores.get(String(alloc, "alice")) != 10) { return 3; }
        if (back.items.size() != 1) { return 4; }
        if (back.items.get(3).id != 3) { return 5; }
        if (back.items.get(3).label.length() != 5) { return 6; }
      } catch (e: IError) { return -1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules_ProtoImport, proto_imports_generate_dependency_modules) {
  ProtoProject project("sun_proto_imports");
  project
      .addSchema("common.proto",
                 "syntax = \"proto3\";\npackage common;\n"
                 "message Stamp { int64 secs = 1; int32 nanos = 2; }\n"
                 "enum Level { LOW = 0; HIGH = 1; }\n")
      .addSchema(
          "uses.proto",
          "syntax = \"proto3\";\npackage app;\nimport \"common.proto\";\n"
          "message Event { common.Stamp when = 1; common.Level level = 2; "
          "string what = 3; }\n")
      // Only the importing schema is listed; common.proto comes in through it
      .setProgram(R"(
        using std;
        using app;
        using common;
        function main() i32 {
          var alloc = make_heap_allocator();
          var e = Event(alloc);
          e.when.secs = 1700000000;
          e.level = Level.HIGH;
          e.what = String(alloc, "boot");
          var buf = Vec<u8>(alloc, 32);
          e.encode(buf);
          try {
            var back = Event_decode(alloc, buf);
            if (back.when.secs != 1700000000) { return 1; }
            if (proto_enum_to_i32_Level(back.level) != 1) { return 2; }
            if (back.what.length() != 4) { return 3; }
          } catch (e2: IError) { return -1; }
          return 0;
        }
      )",
                  {"schemas/uses.proto"});
  EXPECT_EQ(project.run(), 0);
}

TEST(Modules_ProtoImport, libprotobuf_parses_optional_oneof_map_encoding) {
  ProtoProject project("sun_proto_xv2");
  fs::path outFile = project.file("bytes.bin");
  project.addSchema("t.proto", kFullProto)
      .setProgram(
          "using std;\nusing f;\n"
          "function main() i32 {\n"
          "  var alloc = make_heap_allocator();\n"
          "  var b = Bag(alloc);\n"
          "  b.nickname = Option.Some(String(alloc, \"nick\"));\n"
          "  b.count = Option.Some(0);\n"
          "  var d = Item(alloc);\n  d.id = 9;\n"
          "  b.power = Bag_power.Dock(d);\n"
          "  b.scores.insert(String(alloc, \"alice\"), 10);\n"
          "  var it = Item(alloc);\n  it.id = 3;\n"
          "  it.label = String(alloc, \"three\");\n"
          "  b.items.insert(3, it);\n"
          "  var buf = Vec<u8>(alloc, 64);\n"
          "  b.encode(buf);\n" +
          proto_test::dumpBufferProgramTail(outFile));
  ASSERT_EQ(project.run(), 0);
  std::string bytes = proto_test::readBytes(outFile);
  ASSERT_FALSE(bytes.empty());

  namespace pb = google::protobuf;
  LibprotobufSchema schema(project.schemasDir());
  auto msg = schema.parse("t.proto", "Bag", bytes);
  ASSERT_NE(msg, nullptr) << schema.errors();
  const pb::Descriptor* desc = msg->GetDescriptor();
  const pb::Reflection* refl = msg->GetReflection();
  EXPECT_TRUE(refl->HasField(*msg, desc->FindFieldByName("nickname")));
  EXPECT_EQ(refl->GetString(*msg, desc->FindFieldByName("nickname")), "nick");
  EXPECT_TRUE(refl->HasField(*msg, desc->FindFieldByName("count")));
  EXPECT_EQ(refl->GetInt32(*msg, desc->FindFieldByName("count")), 0);
  const pb::OneofDescriptor* power = desc->FindOneofByName("power");
  ASSERT_NE(power, nullptr);
  const pb::FieldDescriptor* set = refl->GetOneofFieldDescriptor(*msg, power);
  ASSERT_NE(set, nullptr);
  EXPECT_EQ(set->name(), "dock");
  EXPECT_EQ(refl->FieldSize(*msg, desc->FindFieldByName("scores")), 1);
  EXPECT_EQ(refl->FieldSize(*msg, desc->FindFieldByName("items")), 1);
}

// ============================================================================
// Moon export/import: a moon built from a manifest with `protos:` exports
// the synthesized messages; importers need neither the .proto nor libprotoc
// ============================================================================

TEST(Modules_ProtoImport, moon_exports_proto_messages_to_importers) {
  ProtoProject lib("sun_proto_moon1");
  lib.addSchema("telemetry.proto", kTelemetryProto);
  fs::path moonPath = lib.buildMoon("telemetry_lib");
  ASSERT_TRUE(fs::exists(moonPath));

  // The importing program: no .proto anywhere in its manifest or SUN_PATH
  auto imports = getStdlibMoonImports();
  imports.push_back(sun::MoonImport(moonPath.string()));
  auto driver = Driver::createForJIT("proto_moon_app");
  driver->setMoonImports(imports);
  auto value = driver->executeString(R"(
    using std;
    using t;
    function main() i32 {
      var alloc = make_heap_allocator();
      var st = Status(alloc);
      st.robot_id = 7;
      st.name = String(alloc, "rover");
      st.samples.push(300);
      st.mode = Mode.FAULT;
      st.pose.y = -2.0;
      var buf = Vec<u8>(alloc, 32);
      st.encode(buf);
      try {
        var back = Status_decode(alloc, buf);
        if (back.robot_id != 7) { return 1; }
        if (back.name.length() != 5) { return 2; }
        if (back.samples.get_unchecked(0) != 300) { return 3; }
        if (proto_enum_to_i32_Mode(back.mode) != 7) { return 4; }
        if (back.pose.y != -2.0) { return 5; }
      } catch (e: IError) { return -1; }
      return 42;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Modules_ProtoImport, moon_import_plus_same_proto_is_a_collision_error) {
  ProtoProject project("sun_proto_moon2");
  project.addSchema("telemetry.proto", kTelemetryProto);
  fs::path moonPath = project.buildMoon("telemetry_lib");
  // Same schema listed again in a program that already imports the moon
  project.setProgram(
      "using std;\nusing t;\n"
      "function main() i32 { var alloc = make_heap_allocator(); "
      "var s = Status(alloc); return 0; }\n");
  try {
    project.run({sun::MoonImport(moonPath.string())});
    FAIL() << "expected a module collision error";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("collision"), std::string::npos)
        << e.what();
  }
}

TEST(Modules_ProtoImport, moon_exports_nested_dotted_package_modules) {
  // package namo.telemetry -> module namo.telemetry: importers use the
  // dotted path
  ProtoProject lib("sun_proto_moon3");
  lib.addSchema("nested.proto",
                "syntax = \"proto3\";\npackage namo.telemetry;\n"
                "message Ping { int32 seq = 1; }\n");
  fs::path moonPath = lib.buildMoon("nested_lib");

  auto imports = getStdlibMoonImports();
  imports.push_back(sun::MoonImport(moonPath.string()));
  auto app = Driver::createForJIT("proto_moon_nested");
  app->setMoonImports(imports);
  auto value = app->executeString(R"(
    using std;
    using namo.telemetry;
    function main() i32 {
      var alloc = make_heap_allocator();
      var p = Ping(alloc);
      p.seq = 9;
      var buf = Vec<u8>(alloc, 8);
      p.encode(buf);
      try {
        var back = Ping_decode(alloc, buf);
        return back.seq;
      } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 9);
}
