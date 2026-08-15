// tests/test_proto_import.cpp - Native protobuf import: the `protos:`
// manifest key, manifest resolution, and (later milestones) the synthesized
// message classes with encode/decode.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "ast.h"
#include "ast_deserializer.h"
#include "ast_serializer.h"
#include "execution_utils.h"
#include "manifest_processor.h"
#include "metadata_extractor.h"
#include "parser.h"
#include "proto_importer.h"

namespace fs = std::filesystem;

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

TEST(ProtoImportTest, manifest_protos_key_parses) {
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

TEST(ProtoImportTest, manifest_entries_tolerate_trailing_semicolons) {
  std::unique_ptr<BlockExprAST> ast;
  const auto* manifest = parseManifest(R"(
    manifest {
      protos: ["a.proto"];
      moons: ["stdlib.moon"];
      suns: ["helper.sun"];
    }
    function main() i32 { return 0; }
  )",
                                       ast);
  ASSERT_NE(manifest, nullptr);
  EXPECT_EQ(manifest->getProtos().size(), 1u);
  EXPECT_EQ(manifest->getMoons().size(), 1u);
  EXPECT_EQ(manifest->getSuns().size(), 1u);
}

TEST(ProtoImportTest, manifest_protos_mixed_with_other_keys) {
  std::unique_ptr<BlockExprAST> ast;
  const auto* manifest = parseManifest(R"(
    manifest {
      suns: ["helper.sun"]
      protos: ["a.proto"]
      moons: ["stdlib.moon"]
    }
    function main() i32 { return 0; }
  )",
                                       ast);
  ASSERT_NE(manifest, nullptr);
  EXPECT_EQ(manifest->getSuns().size(), 1u);
  EXPECT_EQ(manifest->getProtos().size(), 1u);
  EXPECT_EQ(manifest->getMoons().size(), 1u);
}

TEST(ProtoImportTest, manifest_protos_rejects_non_string_entries) {
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

TEST(ProtoImportTest, manifest_unknown_key_names_protos_in_error) {
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

// ============================================================================
// Serialization round-trip
// ============================================================================

TEST(ProtoImportTest, manifest_protos_serialization_roundtrip) {
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

// ============================================================================
// ManifestProcessor resolution
// ============================================================================

TEST(ProtoImportTest, manifest_processor_resolves_protos_relative_to_base) {
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

TEST(ProtoImportTest, manifest_processor_returns_nullopt_without_manifest) {
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

TEST(ProtoImportTest, wire_varint_roundtrip_small_and_multibyte) {
  auto value = executeStringWithStdlib(R"(
    using sun;
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

TEST(ProtoImportTest, wire_negative_int32_is_ten_bytes_and_roundtrips) {
  auto value = executeStringWithStdlib(R"(
    using sun;
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

TEST(ProtoImportTest, wire_zigzag_sint_roundtrip) {
  auto value = executeStringWithStdlib(R"(
    using sun;
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

TEST(ProtoImportTest, wire_fixed_and_float_roundtrip) {
  auto value = executeStringWithStdlib(R"(
    using sun;
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

TEST(ProtoImportTest, wire_string_and_bytes_roundtrip) {
  auto value = executeStringWithStdlib(R"(
    using sun;
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

TEST(ProtoImportTest, wire_tags_limits_and_skip_unknown) {
  auto value = executeStringWithStdlib(R"(
    using sun;
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
        if (proto_tag_field(t1) != 3) { return -1; }
        if (proto_tag_wire_type(t1) != 0) { return -2; }
        seen3 = r.read_varint();
        var t2: u64 = r.read_tag();
        r.skip_field(proto_tag_wire_type(t2), t2, unknown);
        var t3: u64 = r.read_tag();
        r.skip_field(proto_tag_wire_type(t3), t3, unknown);
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

TEST(ProtoImportTest, wire_truncated_input_throws) {
  auto value = executeStringWithStdlib(R"(
    using sun;
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

TEST(ProtoImportTest, map_string_keys) {
  auto value = executeStringWithStdlib(R"(
    using sun;
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

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include "driver.h"

namespace {

// Write a .proto into a temp dir and JIT a program that imports it via a
// manifest, returning main()'s result.
sun::SunValue runWithProto(const std::string& dirName, const std::string& proto,
                           const std::string& program) {
  initTestEnvironment();
  fs::path dir = fs::temp_directory_path() / dirName;
  fs::create_directories(dir / "schemas");
  {
    std::ofstream out(dir / "schemas" / "t.proto");
    out << proto;
  }
  fs::path entry = dir / "main.sun";
  {
    std::ofstream out(entry);
    out << "manifest { protos: [\"schemas/t.proto\"] }\n" << program;
  }
  auto driver = Driver::createForJIT("proto_test");
  driver->setMoonImports(getStdlibMoonImports());
  return driver->executeFile(entry.string(), 0, nullptr);
}

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

TEST(ProtoImportTest, message_roundtrip_all_field_kinds) {
  auto value = runWithProto("sun_proto_rt1", kTelemetryProto, R"(
    using sun;
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

TEST(ProtoImportTest, zero_values_encode_to_empty_and_decode_defaults) {
  auto value = runWithProto("sun_proto_rt2", kTelemetryProto, R"(
    using sun;
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

TEST(ProtoImportTest, unknown_fields_survive_reencode) {
  auto value = runWithProto("sun_proto_rt3", kTelemetryProto, R"(
    using sun;
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

TEST(ProtoImportTest, unpacked_repeated_scalars_decode) {
  auto value = runWithProto("sun_proto_rt4", kTelemetryProto, R"(
    using sun;
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

TEST(ProtoImportTest, unknown_enum_value_maps_to_zero_variant) {
  auto value = runWithProto("sun_proto_rt5", kTelemetryProto, R"(
    using sun;
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

TEST(ProtoImportTest, truncated_message_throws_decode_error) {
  auto value = runWithProto("sun_proto_rt6", kTelemetryProto, R"(
    using sun;
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

TEST(ProtoImportTest, nested_message_types_flatten_with_underscore) {
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
  )", R"(
    using sun;
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

TEST(ProtoImportTest, delimited_stream_framing) {
  auto value = runWithProto("sun_proto_rt8", kTelemetryProto, R"(
    using sun;
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

TEST(ProtoImportTest, proto2_syntax_is_rejected) {
  EXPECT_THROW(runWithProto("sun_proto_rej1", R"(
    syntax = "proto2";
    package p;
    message M { optional int32 a = 1; }
  )", "function main() i32 { return 0; }"), std::exception);
}

TEST(ProtoImportTest, recursive_message_is_rejected) {
  try {
    runWithProto("sun_proto_rej2", R"(
      syntax = "proto3";
      package p;
      message Node { Node child = 1; }
    )", "function main() i32 { return 0; }");
    FAIL() << "expected rejection";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("recursive"), std::string::npos);
  }
}

TEST(ProtoImportTest, proto_syntax_error_reports_file_line_column) {
  try {
    runWithProto("sun_proto_rej3", R"(
      syntax = "proto3";
      package p;
      message M { int32 a = ; }
    )", "function main() i32 { return 0; }");
    FAIL() << "expected rejection";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("t.proto:4:"), std::string::npos) << msg;
  }
}

TEST(ProtoImportTest, missing_proto_file_is_reported) {
  initTestEnvironment();
  fs::path dir = fs::temp_directory_path() / "sun_proto_rej4";
  fs::create_directories(dir);
  fs::path entry = dir / "main.sun";
  {
    std::ofstream out(entry);
    out << "manifest { protos: [\"nope.proto\"] }\nfunction main() i32 { return 0; }\n";
  }
  auto driver = Driver::createForJIT("proto_test");
  driver->setMoonImports(getStdlibMoonImports());
  try {
    driver->executeFile(entry.string(), 0, nullptr);
    FAIL() << "expected rejection";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("nope.proto"), std::string::npos);
  }
}

// ============================================================================
// Cross-validation against libprotobuf (linked into the test binary)
// ============================================================================

namespace {

// Bytes of a Status encoded by Sun with the values used in
// message_roundtrip_all_field_kinds, dumped by a Sun program as decimal
// lines. Returned as a byte string.
std::string sunEncodedStatusBytes() {
  initTestEnvironment();
  fs::path dir = fs::temp_directory_path() / "sun_proto_xv";
  fs::create_directories(dir / "schemas");
  {
    std::ofstream out(dir / "schemas" / "t.proto");
    out << kTelemetryProto;
  }
  fs::path outFile = dir / "bytes.txt";
  fs::path entry = dir / "main.sun";
  {
    std::ofstream out(entry);
    out << "manifest { protos: [\"schemas/t.proto\"] }\n"
        << "using sun;\nusing t;\n"
        << "function main() i32 {\n"
        << "  var alloc = make_heap_allocator();\n"
        << "  var st = Status(alloc);\n"
        << "  st.robot_id = 7;\n"
        << "  st.name = String(alloc, \"rover\");\n"
        << "  st.samples.push(1);\n  st.samples.push(300);\n"
        << "  st.mode = Mode.FAULT;\n"
        << "  st.pose.x = 1.5;\n  st.pose.y = -2.0;\n"
        << "  st.tags.push(String(alloc, \"a\"));\n"
        << "  st.ok = true;\n  st.delta = -3;\n  st.crc = 305419896;\n"
        << "  var buf = Vec<u8>(alloc, 64);\n"
        << "  st.encode(buf);\n"
        << "  var fd: i32 = unsafe { __file_open(\"" << outFile.string()
        << "\", 1); };\n"
        << "  if (fd < 0) { return 1; }\n"
        << "  unsafe { __write(fd, buf.rawData(), buf.size()); };\n"
        << "  unsafe { __file_close(fd); };\n"
        << "  return 0;\n}\n";
  }
  auto driver = Driver::createForJIT("proto_xv");
  driver->setMoonImports(getStdlibMoonImports());
  driver->executeFile(entry.string(), 0, nullptr);
  std::ifstream in(outFile, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST(ProtoImportTest, libprotobuf_parses_sun_encoded_message) {
  std::string bytes = sunEncodedStatusBytes();
  ASSERT_FALSE(bytes.empty());

  // Build the descriptor with libprotoc from the same schema text
  namespace pb = google::protobuf;
  namespace pbc = google::protobuf::compiler;
  fs::path dir = fs::temp_directory_path() / "sun_proto_xv" / "schemas";
  pbc::DiskSourceTree tree;
  tree.MapPath("", dir.string());
  struct Collector : pbc::MultiFileErrorCollector {
    void AddError(const std::string&, int, int, const std::string& m) override {
      msgs += m;
    }
    std::string msgs;
  } errors;
  pbc::Importer importer(&tree, &errors);
  const pb::FileDescriptor* file = importer.Import("t.proto");
  ASSERT_NE(file, nullptr) << errors.msgs;
  const pb::Descriptor* desc = file->FindMessageTypeByName("Status");
  ASSERT_NE(desc, nullptr);

  pb::DynamicMessageFactory factory;
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(desc)->New());
  ASSERT_TRUE(msg->ParseFromString(bytes));
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
  const pb::Descriptor* poseDesc = pose.GetDescriptor();
  EXPECT_DOUBLE_EQ(pose.GetReflection()->GetDouble(
                       pose, poseDesc->FindFieldByName("y")),
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

TEST(ProtoImportTest, optional_fields_track_presence) {
  auto value = runWithProto("sun_proto_full1", kFullProto, R"(
    using sun;
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

TEST(ProtoImportTest, oneof_roundtrips_each_variant) {
  auto value = runWithProto("sun_proto_full2", kFullProto, R"(
    using sun;
    using f;
    function roundtrip(alloc: ref HeapAllocator, b: ref Bag) i32, IError {
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

TEST(ProtoImportTest, oneof_last_field_on_wire_wins) {
  auto value = runWithProto("sun_proto_full3", kFullProto, R"(
    using sun;
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

TEST(ProtoImportTest, maps_with_string_keys_and_message_values) {
  auto value = runWithProto("sun_proto_full4", kFullProto, R"(
    using sun;
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

TEST(ProtoImportTest, proto_imports_generate_dependency_modules) {
  initTestEnvironment();
  fs::path dir = fs::temp_directory_path() / "sun_proto_imports";
  fs::create_directories(dir / "schemas");
  {
    std::ofstream out(dir / "schemas" / "common.proto");
    out << "syntax = \"proto3\";\npackage common;\n"
           "message Stamp { int64 secs = 1; int32 nanos = 2; }\n"
           "enum Level { LOW = 0; HIGH = 1; }\n";
  }
  {
    std::ofstream out(dir / "schemas" / "uses.proto");
    out << "syntax = \"proto3\";\npackage app;\nimport \"common.proto\";\n"
           "message Event { common.Stamp when = 1; common.Level level = 2; "
           "string what = 3; }\n";
  }
  fs::path entry = dir / "main.sun";
  {
    std::ofstream out(entry);
    out << R"(
      manifest { protos: ["schemas/uses.proto"] }
      using sun;
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
    )";
  }
  auto driver = Driver::createForJIT("proto_test");
  driver->setMoonImports(getStdlibMoonImports());
  auto value = driver->executeFile(entry.string(), 0, nullptr);
  EXPECT_EQ(value, 0);
}

TEST(ProtoImportTest, libprotobuf_parses_optional_oneof_map_encoding) {
  // Bytes produced by Sun for optional + oneof(message) + map<int64, Item>
  initTestEnvironment();
  fs::path dir = fs::temp_directory_path() / "sun_proto_xv2";
  fs::create_directories(dir / "schemas");
  {
    std::ofstream out(dir / "schemas" / "t.proto");
    out << kFullProto;
  }
  fs::path outFile = dir / "bytes.bin";
  fs::path entry = dir / "main.sun";
  {
    std::ofstream out(entry);
    out << "manifest { protos: [\"schemas/t.proto\"] }\n"
        << "using sun;\nusing f;\n"
        << "function main() i32 {\n"
        << "  var alloc = make_heap_allocator();\n"
        << "  var b = Bag(alloc);\n"
        << "  b.nickname = Option.Some(String(alloc, \"nick\"));\n"
        << "  b.count = Option.Some(0);\n"
        << "  var d = Item(alloc);\n  d.id = 9;\n"
        << "  b.power = Bag_power.Dock(d);\n"
        << "  b.scores.insert(String(alloc, \"alice\"), 10);\n"
        << "  var it = Item(alloc);\n  it.id = 3;\n"
        << "  it.label = String(alloc, \"three\");\n"
        << "  b.items.insert(3, it);\n"
        << "  var buf = Vec<u8>(alloc, 64);\n"
        << "  b.encode(buf);\n"
        << "  var fd: i32 = unsafe { __file_open(\"" << outFile.string()
        << "\", 1); };\n"
        << "  if (fd < 0) { return 1; }\n"
        << "  unsafe { __write(fd, buf.rawData(), buf.size()); };\n"
        << "  unsafe { __file_close(fd); };\n"
        << "  return 0;\n}\n";
  }
  auto driver = Driver::createForJIT("proto_xv2");
  driver->setMoonImports(getStdlibMoonImports());
  ASSERT_EQ(driver->executeFile(entry.string(), 0, nullptr), 0);
  std::string bytes;
  {
    std::ifstream in(outFile, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    bytes = ss.str();
  }
  ASSERT_FALSE(bytes.empty());

  namespace pb = google::protobuf;
  namespace pbc = google::protobuf::compiler;
  pbc::DiskSourceTree tree;
  tree.MapPath("", (dir / "schemas").string());
  struct Collector : pbc::MultiFileErrorCollector {
    void AddError(const std::string&, int, int, const std::string& m) override {
      msgs += m;
    }
    std::string msgs;
  } errors;
  pbc::Importer importer(&tree, &errors);
  const pb::FileDescriptor* file = importer.Import("t.proto");
  ASSERT_NE(file, nullptr) << errors.msgs;
  const pb::Descriptor* desc = file->FindMessageTypeByName("Bag");
  pb::DynamicMessageFactory factory;
  std::unique_ptr<pb::Message> msg(factory.GetPrototype(desc)->New());
  ASSERT_TRUE(msg->ParseFromString(bytes));
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

#include "moon/moon.h"

namespace {

// Build <dir>/lib/telemetry_lib.moon from a manifest listing telemetry.proto
fs::path buildTelemetryMoon(const std::string& dirName) {
  initTestEnvironment();
  fs::path dir = fs::temp_directory_path() / dirName;
  fs::create_directories(dir / "lib");
  {
    std::ofstream out(dir / "lib" / "telemetry.proto");
    out << kTelemetryProto;
  }
  fs::path entry = dir / "lib" / "telemetry_lib.sun";
  {
    std::ofstream out(entry);
    out << "manifest { protos: [\"telemetry.proto\"] }\n";
  }
  // Same steps as `sun --emit-moon`
  auto manifest = sun::ManifestProcessor::fromEntrypointFile(entry.string());
  std::vector<sun::moon::ModuleMetadata> allMetadata;
  std::vector<std::string> importDirs{(dir / "lib").string()};
  for (const auto& protoPath : manifest->protoFiles) {
    auto synthesized = sun::ProtoImporter::import(protoPath, importDirs);
    auto md = sun::extractAllMetadataFromSource(
        synthesized.sunSource, synthesized.pseudoPath, (dir / "lib").string());
    for (auto& m : *md) allMetadata.push_back(std::move(m));
  }
  auto driver = Driver::createForAOT("moon_module");
  driver->compileFiles({entry.string()}, getStdlibMoonImports(),
                       manifest->protoFiles);
  sun::SunLibWriter writer;
  for (auto& md : allMetadata) writer.addModule(driver->getModule(), md);
  fs::path moonPath = dir / "lib" / "telemetry_lib.moon";
  writer.write(moonPath);
  return moonPath;
}

}  // namespace

TEST(ProtoImportTest, moon_exports_proto_messages_to_importers) {
  fs::path moonPath = buildTelemetryMoon("sun_proto_moon1");
  ASSERT_TRUE(fs::exists(moonPath));

  // The importing program: no .proto anywhere in its manifest or SUN_PATH
  auto imports = getStdlibMoonImports();
  imports.push_back(sun::MoonImport(moonPath.string()));
  auto driver = Driver::createForJIT("proto_moon_app");
  driver->setMoonImports(imports);
  auto value = driver->executeString(R"(
    using sun;
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

TEST(ProtoImportTest, moon_import_plus_same_proto_is_a_collision_error) {
  fs::path moonPath = buildTelemetryMoon("sun_proto_moon2");
  fs::path dir = moonPath.parent_path().parent_path();
  fs::path entry = dir / "main.sun";
  {
    std::ofstream out(entry);
    out << "manifest { protos: [\"lib/telemetry.proto\"] moons: [\""
        << moonPath.string() << "\"] }\n"
        << "using sun;\nusing t;\n"
        << "function main() i32 { var alloc = make_heap_allocator(); "
        << "var s = Status(alloc); return 0; }\n";
  }
  auto driver = Driver::createForJIT("proto_moon_dup");
  driver->setMoonImports(getStdlibMoonImports());
  try {
    driver->executeFile(entry.string(), 0, nullptr);
    FAIL() << "expected a module collision error";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("collision"), std::string::npos)
        << e.what();
  }
}

TEST(ProtoImportTest, moon_exports_nested_dotted_package_modules) {
  // package namo.telemetry -> module namo.telemetry: importers use the
  // dotted path
  initTestEnvironment();
  fs::path dir = fs::temp_directory_path() / "sun_proto_moon3";
  fs::create_directories(dir / "lib");
  {
    std::ofstream out(dir / "lib" / "nested.proto");
    out << "syntax = \"proto3\";\npackage namo.telemetry;\n"
           "message Ping { int32 seq = 1; }\n";
  }
  fs::path entry = dir / "lib" / "nested_lib.sun";
  {
    std::ofstream out(entry);
    out << "manifest { protos: [\"nested.proto\"] }\n";
  }
  auto manifest = sun::ManifestProcessor::fromEntrypointFile(entry.string());
  std::vector<sun::moon::ModuleMetadata> allMetadata;
  for (const auto& protoPath : manifest->protoFiles) {
    auto synthesized =
        sun::ProtoImporter::import(protoPath, {(dir / "lib").string()});
    auto md = sun::extractAllMetadataFromSource(
        synthesized.sunSource, synthesized.pseudoPath, (dir / "lib").string());
    for (auto& m : *md) allMetadata.push_back(std::move(m));
  }
  ASSERT_EQ(allMetadata.size(), 1u);
  EXPECT_EQ(allMetadata[0].module_name(), "namo.telemetry");
  auto driver = Driver::createForAOT("moon_module");
  driver->compileFiles({entry.string()}, getStdlibMoonImports(),
                       manifest->protoFiles);
  sun::SunLibWriter writer;
  for (auto& md : allMetadata) writer.addModule(driver->getModule(), md);
  fs::path moonPath = dir / "lib" / "nested_lib.moon";
  ASSERT_TRUE(writer.write(moonPath));

  auto imports = getStdlibMoonImports();
  imports.push_back(sun::MoonImport(moonPath.string()));
  auto app = Driver::createForJIT("proto_moon_nested");
  app->setMoonImports(imports);
  auto value = app->executeString(R"(
    using sun;
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
