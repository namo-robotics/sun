// proto_importer.cpp — see proto_importer.h
//
// Structure:
//   SchemaValidator   rejects constructs outside the supported proto3 subset
//   TypeMapper        proto descriptor → Sun spelling (types, names, wire types,
//                     read/write expressions)
//   MessageGenerator  emits the Sun class + decode functions for one message
//   emitFile          emits one .proto file's module (enums + messages)
//   ProtoImporter     public API: parse with libprotoc, order dependencies,
//                     generate one Sun source per manifest entry
//
// Generated shape (proto3):
//
//   module <package> {
//     using sun;
//     enum <Enum> { A, B, ... }                    // proto enums
//     class <Msg> {
//       var <field>: <SunType>; ...
//       var unknown_fields: Vec<u8>;               // preserved unknown fields
//       var alloc_: HeapAllocator;
//       function init(alloc: ref HeapAllocator) { ...zero values... }
//       function encode(buf: ref Vec<u8>) void { ... }
//     }
//     function <Msg>_decode(alloc: ref HeapAllocator, buf: ref Vec<u8>) <Msg>, IError
//   }
//
// Nested messages/enums flatten to Outer_Inner. Sun has no static methods, so
// decoding is a free function per message.

#include "moon_bundling/proto_importer.h"

#include <llvm/Support/SHA256.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>

#include "support/error.h"
#include "support/sun_path.h"

namespace sun {

namespace {

namespace pb = google::protobuf;
namespace pbc = google::protobuf::compiler;
using FD = pb::FieldDescriptor;

[[noreturn]] void fail(const std::string& message) {
  throw SunError(SunError::Kind::Compile, "proto import: " + message);
}

// ---------------------------------------------------------------------------
// Diagnostics: proto parse errors surface as one Sun compile error
// ---------------------------------------------------------------------------

class ErrorCollector : public pbc::MultiFileErrorCollector {
 public:
  void AddError(const std::string& filename, int line, int column,
                const std::string& message) override {
    std::ostringstream os;
    os << filename << ":" << (line + 1) << ":" << (column + 1) << ": "
       << message;
    errors_.push_back(os.str());
  }
  bool hasErrors() const { return !errors_.empty(); }
  std::string joined() const {
    std::string out;
    for (const auto& e : errors_) {
      if (!out.empty()) out += "\n";
      out += e;
    }
    return out;
  }

 private:
  std::vector<std::string> errors_;
};

// ---------------------------------------------------------------------------
// Indenting source writer
// ---------------------------------------------------------------------------

class Writer {
 public:
  void line(const std::string& text = "") {
    if (text.empty()) {
      out_ += "\n";
      return;
    }
    out_ += std::string(indent_ * 2, ' ') + text + "\n";
  }
  void open(const std::string& text) {
    line(text);
    ++indent_;
  }
  void close(const std::string& text = "}") {
    --indent_;
    line(text);
  }
  // Pop an indent level without emitting a brace (the next opener prints
  // "} else if (...) {" itself)
  void closeSilently() { --indent_; }
  const std::string& str() const { return out_; }

 private:
  std::string out_;
  int indent_ = 0;
};

// ---------------------------------------------------------------------------
// Descriptor traversal helpers
// ---------------------------------------------------------------------------

// Every message in the file, nested ones included (pre-order)
std::vector<const pb::Descriptor*> allMessages(const pb::FileDescriptor* file) {
  std::vector<const pb::Descriptor*> out;
  std::vector<const pb::Descriptor*> stack;
  for (int i = 0; i < file->message_type_count(); ++i) {
    stack.push_back(file->message_type(i));
  }
  while (!stack.empty()) {
    const pb::Descriptor* d = stack.back();
    stack.pop_back();
    out.push_back(d);
    for (int i = 0; i < d->nested_type_count(); ++i) {
      stack.push_back(d->nested_type(i));
    }
  }
  return out;
}

// Every enum in the file, nested ones included
std::vector<const pb::EnumDescriptor*> allEnums(const pb::FileDescriptor* file) {
  std::vector<const pb::EnumDescriptor*> out;
  for (int i = 0; i < file->enum_type_count(); ++i) {
    out.push_back(file->enum_type(i));
  }
  for (const pb::Descriptor* d : allMessages(file)) {
    for (int i = 0; i < d->enum_type_count(); ++i) out.push_back(d->enum_type(i));
  }
  return out;
}

// Messages in dependency order: a message's by-value sub-messages (from the
// same file) and nested types come first
std::vector<const pb::Descriptor*> messagesInDependencyOrder(
    const pb::FileDescriptor* file) {
  std::vector<const pb::Descriptor*> out;
  std::set<const pb::Descriptor*> done;
  std::function<void(const pb::Descriptor*)> visit =
      [&](const pb::Descriptor* d) {
        if (!done.insert(d).second) return;
        for (int i = 0; i < d->nested_type_count(); ++i) visit(d->nested_type(i));
        for (int i = 0; i < d->field_count(); ++i) {
          const FD* f = d->field(i);
          if (f->type() == FD::TYPE_MESSAGE &&
              f->message_type()->file() == file) {
            visit(f->message_type());
          }
        }
        out.push_back(d);
      };
  for (int i = 0; i < file->message_type_count(); ++i) {
    visit(file->message_type(i));
  }
  return out;
}

// ---------------------------------------------------------------------------
// SchemaValidator: the supported subset, with precise rejections
// ---------------------------------------------------------------------------

class SchemaValidator {
 public:
  static void validate(const pb::FileDescriptor* file) {
    if (file->syntax() != pb::FileDescriptor::SYNTAX_PROTO3) {
      fail("'" + file->name() +
           "' must use syntax = \"proto3\" (proto2 is not supported)");
    }
    if (file->extension_count() > 0) {
      fail("extensions are not supported ('" + file->name() + "')");
    }
    for (const pb::Descriptor* d : allMessages(file)) {
      for (int i = 0; i < d->field_count(); ++i) validateField(d->field(i));
    }
    rejectRecursiveMessages(file);
  }

 private:
  static void validateField(const FD* f) {
    if (f->type() == FD::TYPE_GROUP) {
      fail("groups are not supported (field '" + f->full_name() + "')");
    }
    if (f->is_map() && !isSupportedMapKey(f->message_type()->map_key())) {
      fail("unsupported map key type on field '" + f->full_name() +
           "' (integer and string keys are supported)");
    }
  }

  static bool isSupportedMapKey(const FD* key) {
    switch (key->type()) {
      case FD::TYPE_INT32:
      case FD::TYPE_INT64:
      case FD::TYPE_SINT32:
      case FD::TYPE_SINT64:
      case FD::TYPE_SFIXED32:
      case FD::TYPE_SFIXED64:
      case FD::TYPE_UINT32:
      case FD::TYPE_UINT64:
      case FD::TYPE_FIXED32:
      case FD::TYPE_FIXED64:
      case FD::TYPE_STRING:
        return true;
      default:
        return false;
    }
  }

  // A message that embeds itself by value (directly or through other
  // messages) has no finite layout. `repeated` breaks the cycle (heap).
  static void rejectRecursiveMessages(const pb::FileDescriptor* file) {
    for (const pb::Descriptor* root : allMessages(file)) {
      std::set<const pb::Descriptor*> visited;
      std::vector<const pb::Descriptor*> dfs{root};
      while (!dfs.empty()) {
        const pb::Descriptor* d = dfs.back();
        dfs.pop_back();
        for (int i = 0; i < d->field_count(); ++i) {
          const FD* f = d->field(i);
          if (f->type() != FD::TYPE_MESSAGE || f->is_repeated()) continue;
          const pb::Descriptor* sub = f->message_type();
          if (sub == root) {
            fail("message '" + root->full_name() +
                 "' embeds itself by value (through field '" + f->full_name() +
                 "'); recursive messages are not supported");
          }
          if (visited.insert(sub).second) dfs.push_back(sub);
        }
      }
    }
  }
};

// ---------------------------------------------------------------------------
// TypeMapper: proto descriptor → Sun spelling
// ---------------------------------------------------------------------------

class TypeMapper {
 public:
  // Sun identifier for a (possibly nested) message: Outer_Inner
  static std::string messageName(const pb::Descriptor* d) {
    std::string name = d->name();
    for (const pb::Descriptor* p = d->containing_type(); p;
         p = p->containing_type()) {
      name = p->name() + "_" + name;
    }
    return name;
  }

  static std::string enumName(const pb::EnumDescriptor* e) {
    std::string name = e->name();
    for (const pb::Descriptor* p = e->containing_type(); p;
         p = p->containing_type()) {
      name = p->name() + "_" + name;
    }
    return name;
  }

  // Synthesized payload enum for a oneof: <Msg>_<oneof>
  static std::string oneofEnumName(const pb::OneofDescriptor* o) {
    return messageName(o->containing_type()) + "_" + o->name();
  }

  // Variant name for a oneof member: snake_case field → CamelCase
  static std::string oneofVariantName(const FD* f) {
    std::string out;
    bool upper = true;
    for (char c : f->name()) {
      if (c == '_') {
        upper = true;
        continue;
      }
      out += upper ? static_cast<char>(std::toupper(c)) : c;
      upper = false;
    }
    return out;
  }

  // Sun type of one element of the field (ignoring repeated/optional/map)
  static std::string elementType(const FD* f) {
    switch (f->type()) {
      case FD::TYPE_INT32:
      case FD::TYPE_SINT32:
      case FD::TYPE_SFIXED32:
        return "i32";
      case FD::TYPE_INT64:
      case FD::TYPE_SINT64:
      case FD::TYPE_SFIXED64:
        return "i64";
      case FD::TYPE_UINT32:
      case FD::TYPE_FIXED32:
        return "u32";
      case FD::TYPE_UINT64:
      case FD::TYPE_FIXED64:
        return "u64";
      case FD::TYPE_FLOAT:
        return "f32";
      case FD::TYPE_DOUBLE:
        return "f64";
      case FD::TYPE_BOOL:
        return "bool";
      case FD::TYPE_STRING:
        return "String";
      case FD::TYPE_BYTES:
        return "Vec<u8>";
      case FD::TYPE_ENUM:
        return enumName(f->enum_type());
      case FD::TYPE_MESSAGE:
        return messageName(f->message_type());
      default:
        return "";
    }
  }

  // Sun type of the field as declared on the class
  static std::string fieldType(const FD* f) {
    if (f->is_map()) {
      const pb::Descriptor* entry = f->message_type();
      return "Map<" + elementType(entry->map_key()) + ", " +
             elementType(entry->map_value()) + ">";
    }
    if (f->is_repeated()) return "Vec<" + elementType(f) + ">";
    if (f->has_optional_keyword()) return "Option<" + elementType(f) + ">";
    return elementType(f);
  }

  // Wire type of one element (0 varint, 1 64-bit, 2 length-delimited, 5 32-bit)
  static int wireType(const FD* f) {
    switch (f->type()) {
      case FD::TYPE_FIXED64:
      case FD::TYPE_SFIXED64:
      case FD::TYPE_DOUBLE:
        return 1;
      case FD::TYPE_STRING:
      case FD::TYPE_BYTES:
      case FD::TYPE_MESSAGE:
        return 2;
      case FD::TYPE_FIXED32:
      case FD::TYPE_SFIXED32:
      case FD::TYPE_FLOAT:
        return 5;
      default:
        return 0;
    }
  }

  // Scalars pack when repeated; strings/bytes/messages never do
  static bool isPackable(const FD* f) {
    switch (f->type()) {
      case FD::TYPE_STRING:
      case FD::TYPE_BYTES:
      case FD::TYPE_MESSAGE:
        return false;
      default:
        return true;
    }
  }

  // Zero-value expression for one element (`alloc` must be in scope)
  static std::string zeroValue(const FD* f) {
    switch (f->type()) {
      case FD::TYPE_FLOAT:
      case FD::TYPE_DOUBLE:
        return "0.0";
      case FD::TYPE_BOOL:
        return "false";
      case FD::TYPE_STRING:
        return "String(alloc, \"\")";
      case FD::TYPE_BYTES:
        return "Vec<u8>(alloc, 4)";
      case FD::TYPE_ENUM:
        return enumName(f->enum_type()) + "." + zeroVariant(f->enum_type());
      case FD::TYPE_MESSAGE:
        return messageName(f->message_type()) + "(alloc)";
      default:
        return "0";
    }
  }

  // Expression that reads one element from reader `r`
  static std::string readExpr(const FD* f) {
    switch (f->type()) {
      case FD::TYPE_STRING:
        return "r.read_string_field(alloc)";
      case FD::TYPE_BYTES:
        return "r.read_bytes_field(alloc)";
      case FD::TYPE_MESSAGE:
        return messageName(f->message_type()) + "_decode_nested(alloc, r)";
      case FD::TYPE_ENUM:
        return "proto_enum_from_i32_" + enumName(f->enum_type()) +
               "(r.read_int32())";
      default:
        return "r.read_" + wireSuffix(f) + "()";
    }
  }

  // Statement writing one element `v` into buffer `dst` (tag already written)
  static std::string writeStmt(const FD* f, const std::string& v,
                               const std::string& dst = "buf") {
    switch (f->type()) {
      case FD::TYPE_STRING:
        return "proto_write_string(" + dst + ", " + v + ");";
      case FD::TYPE_BYTES:
        return "proto_write_bytes(" + dst + ", " + v + ");";
      case FD::TYPE_MESSAGE:
        return v + ".encode_nested(" + dst + ");";
      case FD::TYPE_ENUM:
        return "proto_write_int32(" + dst + ", proto_enum_to_i32_" +
               enumName(f->enum_type()) + "(" + v + "));";
      default:
        return "proto_write_" + wireSuffix(f) + "(" + dst + ", " + v + ");";
    }
  }

  // Non-zero test for a singular field (proto3 implicit presence)
  static std::string nonZeroTest(const FD* f, const std::string& v) {
    switch (f->type()) {
      case FD::TYPE_FLOAT:
      case FD::TYPE_DOUBLE:
        return v + " != 0.0";
      case FD::TYPE_BOOL:
        return v;
      case FD::TYPE_STRING:
        return v + ".length() > 0";
      case FD::TYPE_BYTES:
        return v + ".size() > 0";
      case FD::TYPE_ENUM:
        return "proto_enum_to_i32_" + enumName(f->enum_type()) + "(" + v +
               ") != 0";
      case FD::TYPE_MESSAGE:
        return "true";  // sub-messages are always written
      default:
        return v + " != 0";
    }
  }

  // Name of the enum value numbered 0 (proto3 requires one; fall back to
  // the first declared)
  static std::string zeroVariant(const pb::EnumDescriptor* e) {
    for (int i = 0; i < e->value_count(); ++i) {
      if (e->value(i)->number() == 0) return e->value(i)->name();
    }
    return e->value(0)->name();
  }

 private:
  // proto_write_<suffix> / r.read_<suffix> for a scalar field
  static std::string wireSuffix(const FD* f) {
    switch (f->type()) {
      case FD::TYPE_INT32:
        return "int32";
      case FD::TYPE_INT64:
        return "int64";
      case FD::TYPE_UINT32:
        return "uint32";
      case FD::TYPE_UINT64:
        return "uint64";
      case FD::TYPE_SINT32:
        return "sint32";
      case FD::TYPE_SINT64:
        return "sint64";
      case FD::TYPE_FIXED32:
        return "fixed32";
      case FD::TYPE_FIXED64:
        return "fixed64";
      case FD::TYPE_SFIXED32:
        return "sfixed32";
      case FD::TYPE_SFIXED64:
        return "sfixed64";
      case FD::TYPE_FLOAT:
        return "float";
      case FD::TYPE_DOUBLE:
        return "double";
      case FD::TYPE_BOOL:
        return "bool";
      case FD::TYPE_ENUM:
        return "int32";
      default:
        return "";
    }
  }
};

using T = TypeMapper;

// ---------------------------------------------------------------------------
// Enum generation: Sun enum + explicit i32 conversion tables. Sun enums are
// dense tags; proto values are arbitrary numbers, and unknown wire values
// (open enums) map to the zero value.
// ---------------------------------------------------------------------------

void emitEnum(Writer& w, const pb::EnumDescriptor* e) {
  std::string name = T::enumName(e);
  w.open("public enum " + name + " {");
  for (int i = 0; i < e->value_count(); ++i) {
    std::string sep = (i + 1 < e->value_count()) ? "," : "";
    w.line(e->value(i)->name() + sep);
  }
  w.close();
  w.line();

  w.open("public function proto_enum_to_i32_" + name + "(v: " + name + ") i32 {");
  w.open("return match v {");
  for (int i = 0; i < e->value_count(); ++i) {
    std::string sep = (i + 1 < e->value_count()) ? "," : "";
    w.line(name + "." + e->value(i)->name() + " => " +
           std::to_string(e->value(i)->number()) + sep);
  }
  w.close("};");
  w.close();
  w.line();

  w.open("public function proto_enum_from_i32_" + name + "(v: i32) " + name + " {");
  for (int i = 0; i < e->value_count(); ++i) {
    w.line("if (v == " + std::to_string(e->value(i)->number()) + ") { return " +
           name + "." + e->value(i)->name() + "; }");
  }
  w.line("return " + name + "." + T::zeroVariant(e) + ";");
  w.close();
  w.line();
}

// ---------------------------------------------------------------------------
// MessageGenerator: one message → oneof enums, class, decode functions
// ---------------------------------------------------------------------------

class MessageGenerator {
 public:
  MessageGenerator(Writer& w, const pb::Descriptor* d)
      : w_(w), d_(d), name_(T::messageName(d)) {}

  void emit() {
    emitOneofEnums();
    w_.open("public class " + name_ + " {");
    emitFields();
    emitInit();
    emitEncode();
    emitEncodeNested();
    w_.close();  // class
    w_.line();
    emitDecodeFrom();
    emitDecodeHelpers();
  }

 private:
  Writer& w_;
  const pb::Descriptor* d_;
  std::string name_;

  // Fields stored directly on the class (oneof members live in the oneof)
  template <typename Fn>
  void forEachPlainField(Fn fn) const {
    for (int i = 0; i < d_->field_count(); ++i) {
      const FD* f = d_->field(i);
      if (!f->real_containing_oneof()) fn(f);
    }
  }

  template <typename Fn>
  void forEachOneof(Fn fn) const {
    for (int i = 0; i < d_->real_oneof_decl_count(); ++i) fn(d_->oneof_decl(i));
  }

  static std::string tagLine(const std::string& dst, int number, int wire) {
    return "proto_write_tag(" + dst + ", " + std::to_string(number) + ", " +
           std::to_string(wire) + ");";
  }

  // enum <Msg>_<oneof> { NotSet, FieldA(T), FieldB(U) }
  void emitOneofEnums() {
    forEachOneof([&](const pb::OneofDescriptor* o) {
      w_.open("public enum " + T::oneofEnumName(o) + " {");
      w_.line("NotSet,");
      for (int j = 0; j < o->field_count(); ++j) {
        const FD* f = o->field(j);
        std::string sep = (j + 1 < o->field_count()) ? "," : "";
        w_.line(T::oneofVariantName(f) + "(" + T::elementType(f) + ")" + sep);
      }
      w_.close();
      w_.line();
    });
  }

  void emitFields() {
    forEachPlainField([&](const FD* f) {
      w_.line("public var " + f->name() + ": " + T::fieldType(f) + ";");
    });
    forEachOneof([&](const pb::OneofDescriptor* o) {
      w_.line("public var " + o->name() + ": " + T::oneofEnumName(o) + ";");
    });
    w_.line("public var unknown_fields: Vec<u8>;");
    w_.line("var alloc_: HeapAllocator;");
    w_.line();
  }

  // Zero values (proto3 defaults); containers and sub-messages start empty
  void emitInit() {
    w_.open("public function init(alloc: ref HeapAllocator) {");
    w_.line("this.alloc_ = alloc.copy();");
    forEachPlainField([&](const FD* f) {
      std::string init;
      if (f->is_map()) {
        init = T::fieldType(f) + "(alloc, 8)";
      } else if (f->is_repeated()) {
        init = T::fieldType(f) + "(alloc, 4)";
      } else if (f->has_optional_keyword()) {
        init = "Option.None";
      } else {
        init = T::zeroValue(f);
      }
      w_.line("this." + f->name() + " = " + init + ";");
    });
    forEachOneof([&](const pb::OneofDescriptor* o) {
      w_.line("this." + o->name() + " = " + T::oneofEnumName(o) + ".NotSet;");
    });
    w_.line("this.unknown_fields = Vec<u8>(alloc, 4);");
    w_.close();
    w_.line();
  }

  void emitEncode() {
    w_.open("public function encode(buf: ref Vec<u8>) void {");
    forEachPlainField([&](const FD* f) { emitEncodeField(f); });
    forEachOneof([&](const pb::OneofDescriptor* o) { emitEncodeOneof(o); });
    w_.line("proto_append_raw(buf, this.unknown_fields);");
    w_.close();
    w_.line();
  }

  void emitEncodeField(const FD* f) {
    std::string fld = "this." + f->name();
    int number = f->number();
    if (f->is_map()) {
      // Each entry is a length-delimited message { 1: key, 2: value }
      const pb::Descriptor* entry = f->message_type();
      const FD* k = entry->map_key();
      const FD* v = entry->map_value();
      w_.open("for (var i: i64 = 0; i < " + fld + ".capacity(); i = i + 1) {");
      w_.open("if (" + fld + ".is_bucket_occupied(i)) {");
      w_.line("var body = Vec<u8>(this.alloc_, 16);");
      w_.line(tagLine("body", 1, T::wireType(k)));
      w_.line(T::writeStmt(k, fld + ".get_bucket_key(i)", "body"));
      w_.line(tagLine("body", 2, T::wireType(v)));
      w_.line(T::writeStmt(v, fld + ".get_bucket_value(i)", "body"));
      w_.line(tagLine("buf", number, 2));
      w_.line("proto_write_bytes(buf, body);");
      w_.close();
      w_.close();
    } else if (f->has_optional_keyword()) {
      // Explicit presence: written whenever set, even if zero
      w_.open("match " + fld + " {");
      w_.open("Option.Some(v) => {");
      w_.line(tagLine("buf", number, T::wireType(f)));
      w_.line(T::writeStmt(f, "v"));
      w_.close("},");
      w_.line("Option.None => { }");
      w_.close("};");
    } else if (f->is_repeated() && T::isPackable(f)) {
      // Packed: one length-delimited record holding all elements
      w_.open("if (" + fld + ".size() > 0) {");
      w_.line("var body = Vec<u8>(this.alloc_, 16);");
      w_.open("for (var i: i64 = 0; i < " + fld + ".size(); i = i + 1) {");
      w_.line(T::writeStmt(f, fld + ".get_unchecked(i)", "body"));
      w_.close();
      w_.line(tagLine("buf", number, 2));
      w_.line("proto_write_bytes(buf, body);");
      w_.close();
    } else if (f->is_repeated()) {
      w_.open("for (var i: i64 = 0; i < " + fld + ".size(); i = i + 1) {");
      w_.line(tagLine("buf", number, T::wireType(f)));
      w_.line(T::writeStmt(f, fld + ".get_unchecked(i)"));
      w_.close();
    } else {
      w_.open("if (" + T::nonZeroTest(f, fld) + ") {");
      w_.line(tagLine("buf", number, T::wireType(f)));
      w_.line(T::writeStmt(f, fld));
      w_.close();
    }
  }

  // Whichever variant is set is written
  void emitEncodeOneof(const pb::OneofDescriptor* o) {
    std::string en = T::oneofEnumName(o);
    w_.open("match this." + o->name() + " {");
    for (int j = 0; j < o->field_count(); ++j) {
      const FD* f = o->field(j);
      w_.open(en + "." + T::oneofVariantName(f) + "(v) => {");
      w_.line(tagLine("buf", f->number(), T::wireType(f)));
      w_.line(T::writeStmt(f, "v"));
      w_.close("},");
    }
    w_.line(en + ".NotSet => { }");
    w_.close("};");
  }

  // Length-prefixed forms: embedded sub-message and stream framing
  void emitEncodeNested() {
    w_.open("public function encode_nested(buf: ref Vec<u8>) void {");
    w_.line("var body = Vec<u8>(this.alloc_, 16);");
    w_.line("this.encode(body);");
    w_.line("proto_write_bytes(buf, body);");
    w_.close();
    w_.line();
    w_.open("public function encode_delimited(buf: ref Vec<u8>) void {");
    w_.line("this.encode_nested(buf);");
    w_.close();
  }

  // <Msg>_decode_from: the field-dispatch loop over a reader
  void emitDecodeFrom() {
    w_.open("public function " + name_ +
            "_decode_from(alloc: ref HeapAllocator, r: ref ProtoReader) " +
            name_ + ", IError {");
    w_.line("var msg = " + name_ + "(alloc);");
    w_.open("while (r.at_end() == false) {");
    w_.line("var tag: u64 = r.read_tag();");
    w_.line("var field: i64 = proto_read_tag_field(tag);");
    w_.line("var wire: i64 = proto_read_tag_wire_type(tag);");
    bool first = true;
    for (int i = 0; i < d_->field_count(); ++i) {
      const FD* f = d_->field(i);
      w_.open(std::string(first ? "if (" : "} else if (") +
              "field == " + std::to_string(f->number()) + ") {");
      first = false;
      emitDecodeField(f);
      w_.closeSilently();
    }
    w_.open(first ? "if (true) {" : "} else {");
    w_.line("r.skip_field(wire, tag, msg.unknown_fields);");
    w_.close();
    w_.close();  // while
    w_.line("return msg;");
    w_.close();
    w_.line();
  }

  void emitDecodeField(const FD* f) {
    std::string fld = "msg." + f->name();
    if (const pb::OneofDescriptor* o = f->real_containing_oneof()) {
      w_.line("msg." + o->name() + " = " + T::oneofEnumName(o) + "." +
              T::oneofVariantName(f) + "(" + T::readExpr(f) + ");");
    } else if (f->is_map()) {
      emitDecodeMapEntry(f, fld);
    } else if (f->has_optional_keyword()) {
      w_.line(fld + " = Option.Some(" + T::readExpr(f) + ");");
    } else if (f->is_repeated() && T::isPackable(f)) {
      // Accept both packed (wire 2) and unpacked encodings
      w_.open("if (wire == 2) {");
      w_.line("var end: i64 = r.read_length();");
      w_.line("var old: i64 = r.push_limit(end);");
      w_.open("while (r.at_end() == false) {");
      w_.line(fld + ".push(" + T::readExpr(f) + ");");
      w_.close();
      w_.line("r.pop_limit(old);");
      w_.close("} else {");
      w_.open("");
      w_.line(fld + ".push(" + T::readExpr(f) + ");");
      w_.close();
    } else if (f->is_repeated()) {
      w_.line(fld + ".push(" + T::readExpr(f) + ");");
    } else {
      w_.line(fld + " = " + T::readExpr(f) + ";");
    }
  }

  // One map entry: a length-delimited { 1: key, 2: value } record
  void emitDecodeMapEntry(const FD* f, const std::string& fld) {
    const pb::Descriptor* entry = f->message_type();
    const FD* k = entry->map_key();
    const FD* v = entry->map_value();
    w_.line("var end: i64 = r.read_length();");
    w_.line("var old: i64 = r.push_limit(end);");
    w_.line("var key: " + T::elementType(k) + " = " + T::zeroValue(k) + ";");
    w_.line("var val: " + T::elementType(v) + " = " + T::zeroValue(v) + ";");
    w_.open("while (r.at_end() == false) {");
    w_.line("var etag: u64 = r.read_tag();");
    w_.line("var efield: i64 = proto_read_tag_field(etag);");
    w_.open("if (efield == 1) {");
    w_.line("key = " + T::readExpr(k) + ";");
    w_.close("} else if (efield == 2) {");
    w_.open("");
    w_.line("val = " + T::readExpr(v) + ";");
    w_.close("} else {");
    w_.open("");
    w_.line("r.skip_field(proto_read_tag_wire_type(etag), etag, msg.unknown_fields);");
    w_.close();
    w_.close();
    w_.line("r.pop_limit(old);");
    w_.line(fld + ".insert(key, val);");
  }

  // Nested (length-prefixed), whole-buffer, and delimited entry points
  void emitDecodeHelpers() {
    const std::string sig = "(alloc: ref HeapAllocator, r: ref ProtoReader) ";
    w_.open("public function " + name_ + "_decode_nested" + sig + name_ + ", IError {");
    w_.line("var end: i64 = r.read_length();");
    w_.line("var old: i64 = r.push_limit(end);");
    w_.line("var msg = " + name_ + "_decode_from(alloc, r);");
    w_.line("r.pop_limit(old);");
    w_.line("return msg;");
    w_.close();
    w_.line();

    w_.open("public function " + name_ +
            "_decode(alloc: ref HeapAllocator, buf: ref Vec<u8>) " + name_ +
            ", IError {");
    w_.line("var r = ProtoReader(buf);");
    w_.line("return " + name_ + "_decode_from(alloc, r);");
    w_.close();
    w_.line();

    w_.open("public function " + name_ + "_decode_delimited" + sig + name_ +
            ", IError {");
    w_.line("return " + name_ + "_decode_nested(alloc, r);");
    w_.close();
    w_.line();
  }
};

// ---------------------------------------------------------------------------
// One .proto file → one Sun module
// ---------------------------------------------------------------------------

std::string sha256Hex(const std::string& data) {
  llvm::SHA256 h;
  h.update(data);
  auto digest = h.final();
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (auto b : digest) {
    out += hex[(b >> 4) & 15];
    out += hex[b & 15];
  }
  return out;
}

void emitFile(Writer& w, const pb::FileDescriptor* file,
              const std::string& displayPath, const std::string& sourceText) {
  SchemaValidator::validate(file);

  w.line("// generated by sun proto-import v1 from " + displayPath);
  w.line("// sha256: " + sha256Hex(sourceText));
  w.line();

  std::string pkg = file->package();
  if (!pkg.empty()) w.open("public module " + pkg + " {");
  // `using` inside the module: merged ASTs order modules before other
  // top-level statements, so a file-level `using` would bind too late for
  // the class shapes. Imported packages are brought into scope the same way.
  w.line("using sun;");
  std::set<std::string> importedPkgs;
  for (int i = 0; i < file->dependency_count(); ++i) {
    const std::string& dpkg = file->dependency(i)->package();
    if (!dpkg.empty() && dpkg != pkg && importedPkgs.insert(dpkg).second) {
      w.line("using " + dpkg + ";");
    }
  }
  w.line();

  for (const auto* e : allEnums(file)) emitEnum(w, e);
  for (const auto* d : messagesInDependencyOrder(file)) {
    MessageGenerator(w, d).emit();
  }

  if (!pkg.empty()) w.close();
  w.line();
}

std::string readFile(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<std::string> ProtoImporter::importDirsFor(const std::string& baseDir) {
  std::vector<std::string> dirs;
  if (!baseDir.empty()) dirs.push_back(baseDir);
  for (const auto& dir : SunPath::getPaths()) dirs.push_back(dir.string());
  return dirs;
}

std::vector<SynthesizedProtoModule> ProtoImporter::importAll(
    const std::vector<std::string>& protoFiles, const std::string& baseDir) {
  std::vector<SynthesizedProtoModule> out;
  auto dirs = importDirsFor(baseDir);
  for (const auto& protoPath : protoFiles) {
    out.push_back(import(protoPath, dirs));
  }
  return out;
}

SynthesizedProtoModule ProtoImporter::import(
    const std::string& protoPath, const std::vector<std::string>& importDirs) {
  namespace fs = std::filesystem;
  fs::path path = fs::absolute(protoPath);
  if (!fs::exists(path)) fail("file not found: " + protoPath);

  // Map every import dir (and the proto's own directory) onto the virtual
  // root so proto-level `import "x.proto"` resolves like other Sun imports.
  pbc::DiskSourceTree tree;
  tree.MapPath("", path.parent_path().string());
  for (const auto& d : importDirs) tree.MapPath("", d);

  ErrorCollector errors;
  pbc::Importer importer(&tree, &errors);
  const pb::FileDescriptor* file = importer.Import(path.filename().string());
  if (!file || errors.hasErrors()) fail(errors.joined());

  // Dependencies first (deepest first, each once), then the file itself:
  // imported messages/enums become sibling modules in the same synthesized
  // source so `using <pkg>;` inside the importer resolves.
  std::vector<const pb::FileDescriptor*> order;
  std::set<const pb::FileDescriptor*> seen;
  std::function<void(const pb::FileDescriptor*)> visit =
      [&](const pb::FileDescriptor* f) {
        if (!seen.insert(f).second) return;
        for (int i = 0; i < f->dependency_count(); ++i) visit(f->dependency(i));
        order.push_back(f);
      };
  visit(file);

  Writer w;
  for (const pb::FileDescriptor* f : order) {
    std::string diskPath;
    if (tree.VirtualFileToDiskFile(f->name(), &diskPath)) {
      emitFile(w, f, diskPath, readFile(diskPath));
    } else {
      emitFile(w, f, f->name(), f->name());
    }
  }

  SynthesizedProtoModule out;
  out.sunSource = w.str();
  out.pseudoPath = "<proto:" + protoPath + ">";
  out.moduleName = file->package();
  return out;
}

}  // namespace sun
