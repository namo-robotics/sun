// proto_importer.cpp — see proto_importer.h
//
// Generated shape (proto3):
//
//   using sun;
//   module <package> {
//     enum <Enum> { A, B, ... }                    // proto enums
//     class <Msg> {
//       var <field>: <SunType>; ...
//       var unknown_fields: Vec<u8>;                     // preserved unknown fields
//       var alloc_: HeapAllocator;
//       function init(alloc: ref HeapAllocator) { ...zero values... }
//       function encode(buf: ref Vec<u8>) void { ... }
//     }
//     function <Msg>_decode(alloc: ref HeapAllocator, buf: ref Vec<u8>) <Msg>, IError
//     function <Msg>_decode_from(alloc: ref HeapAllocator, r: ref ProtoReader) <Msg>, IError
//   }
//
// Nested messages/enums flatten to Outer_Inner. Sun has no static methods, so
// decoding is a free function per message.

#include "proto_importer.h"

#include <llvm/Support/SHA256.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>

#include "error.h"

namespace sun {

namespace {

namespace pb = google::protobuf;
namespace pbc = google::protobuf::compiler;

// Collects proto parse errors and surfaces them as one Sun compile error.
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

// Simple indenting source writer
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

// Sun identifier for a (possibly nested) message or enum: Outer_Inner
std::string sunTypeName(const pb::Descriptor* d) {
  std::string name = d->name();
  for (const pb::Descriptor* p = d->containing_type(); p;
       p = p->containing_type()) {
    name = p->name() + "_" + name;
  }
  return name;
}

std::string sunEnumName(const pb::EnumDescriptor* e) {
  std::string name = e->name();
  for (const pb::Descriptor* p = e->containing_type(); p;
       p = p->containing_type()) {
    name = p->name() + "_" + name;
  }
  return name;
}

// Sun spelling of a field's element type (ignoring repeated/optional)
std::string scalarSunType(const pb::FieldDescriptor* f) {
  switch (f->type()) {
    case pb::FieldDescriptor::TYPE_INT32:
    case pb::FieldDescriptor::TYPE_SINT32:
    case pb::FieldDescriptor::TYPE_SFIXED32:
      return "i32";
    case pb::FieldDescriptor::TYPE_INT64:
    case pb::FieldDescriptor::TYPE_SINT64:
    case pb::FieldDescriptor::TYPE_SFIXED64:
      return "i64";
    case pb::FieldDescriptor::TYPE_UINT32:
    case pb::FieldDescriptor::TYPE_FIXED32:
      return "u32";
    case pb::FieldDescriptor::TYPE_UINT64:
    case pb::FieldDescriptor::TYPE_FIXED64:
      return "u64";
    case pb::FieldDescriptor::TYPE_FLOAT:
      return "f32";
    case pb::FieldDescriptor::TYPE_DOUBLE:
      return "f64";
    case pb::FieldDescriptor::TYPE_BOOL:
      return "bool";
    case pb::FieldDescriptor::TYPE_STRING:
      return "String";
    case pb::FieldDescriptor::TYPE_BYTES:
      return "Vec<u8>";
    case pb::FieldDescriptor::TYPE_ENUM:
      return sunEnumName(f->enum_type());
    case pb::FieldDescriptor::TYPE_MESSAGE:
      return sunTypeName(f->message_type());
    default:
      return "";
  }
}

// Wire type for a (non-repeated) field's element
int wireType(const pb::FieldDescriptor* f) {
  switch (f->type()) {
    case pb::FieldDescriptor::TYPE_FIXED64:
    case pb::FieldDescriptor::TYPE_SFIXED64:
    case pb::FieldDescriptor::TYPE_DOUBLE:
      return 1;
    case pb::FieldDescriptor::TYPE_STRING:
    case pb::FieldDescriptor::TYPE_BYTES:
    case pb::FieldDescriptor::TYPE_MESSAGE:
      return 2;
    case pb::FieldDescriptor::TYPE_FIXED32:
    case pb::FieldDescriptor::TYPE_SFIXED32:
    case pb::FieldDescriptor::TYPE_FLOAT:
      return 5;
    default:
      return 0;  // varint
  }
}

bool isScalarNumeric(const pb::FieldDescriptor* f) {
  switch (f->type()) {
    case pb::FieldDescriptor::TYPE_STRING:
    case pb::FieldDescriptor::TYPE_BYTES:
    case pb::FieldDescriptor::TYPE_MESSAGE:
      return false;
    default:
      return true;
  }
}

// proto_write_<suffix> / read_<suffix> for a scalar field
std::string wireSuffix(const pb::FieldDescriptor* f) {
  switch (f->type()) {
    case pb::FieldDescriptor::TYPE_INT32:
      return "int32";
    case pb::FieldDescriptor::TYPE_INT64:
      return "int64";
    case pb::FieldDescriptor::TYPE_UINT32:
      return "uint32";
    case pb::FieldDescriptor::TYPE_UINT64:
      return "uint64";
    case pb::FieldDescriptor::TYPE_SINT32:
      return "sint32";
    case pb::FieldDescriptor::TYPE_SINT64:
      return "sint64";
    case pb::FieldDescriptor::TYPE_FIXED32:
      return "fixed32";
    case pb::FieldDescriptor::TYPE_FIXED64:
      return "fixed64";
    case pb::FieldDescriptor::TYPE_SFIXED32:
      return "sfixed32";
    case pb::FieldDescriptor::TYPE_SFIXED64:
      return "sfixed64";
    case pb::FieldDescriptor::TYPE_FLOAT:
      return "float";
    case pb::FieldDescriptor::TYPE_DOUBLE:
      return "double";
    case pb::FieldDescriptor::TYPE_BOOL:
      return "bool";
    case pb::FieldDescriptor::TYPE_ENUM:
      return "int32";
    default:
      return "";
  }
}

// Zero-value expression for a field's element type
std::string zeroValue(const pb::FieldDescriptor* f) {
  switch (f->type()) {
    case pb::FieldDescriptor::TYPE_FLOAT:
    case pb::FieldDescriptor::TYPE_DOUBLE:
      return "0.0";
    case pb::FieldDescriptor::TYPE_BOOL:
      return "false";
    case pb::FieldDescriptor::TYPE_STRING:
      return "String(alloc, \"\")";
    case pb::FieldDescriptor::TYPE_BYTES:
      return "Vec<u8>(alloc, 4)";
    case pb::FieldDescriptor::TYPE_ENUM: {
      // Proto3 enums always have a 0 value: use its name
      const auto* e = f->enum_type();
      for (int i = 0; i < e->value_count(); ++i) {
        if (e->value(i)->number() == 0) {
          return sunEnumName(e) + "." + e->value(i)->name();
        }
      }
      return sunEnumName(e) + "." + e->value(0)->name();
    }
    case pb::FieldDescriptor::TYPE_MESSAGE:
      return sunTypeName(f->message_type()) + "(alloc)";
    default:
      return "0";
  }
}

// Expression that reads one element of field `f` from reader `r`
std::string readExpr(const pb::FieldDescriptor* f) {
  switch (f->type()) {
    case pb::FieldDescriptor::TYPE_STRING:
      return "r.read_string_field(alloc)";
    case pb::FieldDescriptor::TYPE_BYTES:
      return "r.read_bytes_field(alloc)";
    case pb::FieldDescriptor::TYPE_MESSAGE:
      return sunTypeName(f->message_type()) + "_decode_nested(alloc, r)";
    case pb::FieldDescriptor::TYPE_ENUM:
      return "proto_enum_from_i32_" + sunEnumName(f->enum_type()) +
             "(r.read_int32())";
    default:
      return "r.read_" + wireSuffix(f) + "()";
  }
}

// Statement that writes one element `v` of field `f` into buffer `dst`
// (tag already written)
std::string writeStmt(const pb::FieldDescriptor* f, const std::string& v,
                      const std::string& dst = "buf") {
  switch (f->type()) {
    case pb::FieldDescriptor::TYPE_STRING:
      return "proto_write_string(" + dst + ", " + v + ");";
    case pb::FieldDescriptor::TYPE_BYTES:
      return "proto_write_bytes(" + dst + ", " + v + ");";
    case pb::FieldDescriptor::TYPE_MESSAGE:
      return v + ".encode_nested(" + dst + ");";
    case pb::FieldDescriptor::TYPE_ENUM:
      return "proto_write_int32(" + dst + ", proto_enum_to_i32_" +
             sunEnumName(f->enum_type()) + "(" + v + "));";
    default:
      return "proto_write_" + wireSuffix(f) + "(" + dst + ", " + v + ");";
  }
}

// A field's Sun storage type as declared on the class
std::string fieldSunType(const pb::FieldDescriptor* f) {
  if (f->is_map()) {
    const pb::Descriptor* entry = f->message_type();
    return "Map<" + scalarSunType(entry->map_key()) + ", " +
           scalarSunType(entry->map_value()) + ">";
  }
  if (f->is_repeated()) return "Vec<" + scalarSunType(f) + ">";
  if (f->has_optional_keyword()) return "Option<" + scalarSunType(f) + ">";
  return scalarSunType(f);
}

// Synthesized enum name for a oneof: <Msg>_<oneof>
std::string oneofEnumName(const pb::OneofDescriptor* o) {
  return sunTypeName(o->containing_type()) + "_" + o->name();
}

// Variant name for a oneof member: snake_case field -> CamelCase
std::string oneofVariantName(const pb::FieldDescriptor* f) {
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

// Non-zero test for a singular field (proto3 implicit presence)
std::string nonZeroTest(const pb::FieldDescriptor* f, const std::string& v) {
  switch (f->type()) {
    case pb::FieldDescriptor::TYPE_FLOAT:
    case pb::FieldDescriptor::TYPE_DOUBLE:
      return v + " != 0.0";
    case pb::FieldDescriptor::TYPE_BOOL:
      return v;
    case pb::FieldDescriptor::TYPE_STRING:
      return v + ".length() > 0";
    case pb::FieldDescriptor::TYPE_BYTES:
      return v + ".size() > 0";
    case pb::FieldDescriptor::TYPE_ENUM:
      return "proto_enum_to_i32_" + sunEnumName(f->enum_type()) + "(" + v +
             ") != 0";
    case pb::FieldDescriptor::TYPE_MESSAGE:
      return "true";  // sub-messages are always written in v1
    default:
      return v + " != 0";
  }
}

// Reject constructs outside the supported subset with a precise message
void rejectUnsupported(const pb::FileDescriptor* file) {
  if (file->syntax() != pb::FileDescriptor::SYNTAX_PROTO3) {
    throw SunError(SunError::Kind::Compile,
                   "proto import: '" + file->name() +
                       "' must use syntax = \"proto3\" (proto2 is not "
                       "supported)");
  }
  if (file->extension_count() > 0) {
    throw SunError(SunError::Kind::Compile,
                   "proto import: extensions are not supported ('" +
                       file->name() + "')");
  }
  std::vector<const pb::Descriptor*> stack;
  for (int i = 0; i < file->message_type_count(); ++i) {
    stack.push_back(file->message_type(i));
  }
  while (!stack.empty()) {
    const pb::Descriptor* d = stack.back();
    stack.pop_back();
    for (int i = 0; i < d->nested_type_count(); ++i) {
      stack.push_back(d->nested_type(i));
    }
    for (int i = 0; i < d->field_count(); ++i) {
      const pb::FieldDescriptor* f = d->field(i);
      if (f->type() == pb::FieldDescriptor::TYPE_GROUP) {
        throw SunError(SunError::Kind::Compile,
                       "proto import: groups are not supported (field '" +
                           f->full_name() + "')");
      }
      if (f->is_map()) {
        const pb::FieldDescriptor* key = f->message_type()->map_key();
        switch (key->type()) {
          case pb::FieldDescriptor::TYPE_INT32:
          case pb::FieldDescriptor::TYPE_INT64:
          case pb::FieldDescriptor::TYPE_SINT32:
          case pb::FieldDescriptor::TYPE_SINT64:
          case pb::FieldDescriptor::TYPE_SFIXED32:
          case pb::FieldDescriptor::TYPE_SFIXED64:
          case pb::FieldDescriptor::TYPE_UINT32:
          case pb::FieldDescriptor::TYPE_UINT64:
          case pb::FieldDescriptor::TYPE_FIXED32:
          case pb::FieldDescriptor::TYPE_FIXED64:
          case pb::FieldDescriptor::TYPE_STRING:
            break;
          default:
            throw SunError(SunError::Kind::Compile,
                           "proto import: unsupported map key type on field '" +
                               f->full_name() +
                               "' (integer and string keys are supported)");
        }
      }
    }
  }
}

// Message cycles by value cannot be embedded: reject with a clear error
void rejectRecursiveMessages(const pb::FileDescriptor* file) {
  std::vector<const pb::Descriptor*> all;
  std::vector<const pb::Descriptor*> stack;
  for (int i = 0; i < file->message_type_count(); ++i) {
    stack.push_back(file->message_type(i));
  }
  while (!stack.empty()) {
    const pb::Descriptor* d = stack.back();
    stack.pop_back();
    all.push_back(d);
    for (int i = 0; i < d->nested_type_count(); ++i) {
      stack.push_back(d->nested_type(i));
    }
  }
  for (const pb::Descriptor* root : all) {
    std::set<const pb::Descriptor*> visited;
    std::vector<const pb::Descriptor*> dfs{root};
    while (!dfs.empty()) {
      const pb::Descriptor* d = dfs.back();
      dfs.pop_back();
      for (int i = 0; i < d->field_count(); ++i) {
        const pb::FieldDescriptor* f = d->field(i);
        if (f->type() != pb::FieldDescriptor::TYPE_MESSAGE) continue;
        // repeated sub-messages live in a Vec (heap), which breaks the cycle
        if (f->is_repeated()) continue;
        const pb::Descriptor* sub = f->message_type();
        if (sub == root) {
          throw SunError(SunError::Kind::Compile,
                         "proto import: message '" + root->full_name() +
                             "' embeds itself by value (through field '" +
                             f->full_name() +
                             "'); recursive messages are not supported");
        }
        if (visited.insert(sub).second) dfs.push_back(sub);
      }
    }
  }
}

// Messages in dependency order (a message's by-value sub-messages first)
void orderMessages(const pb::FileDescriptor* file,
                   std::vector<const pb::Descriptor*>& out) {
  std::set<const pb::Descriptor*> done;
  std::function<void(const pb::Descriptor*)> visit =
      [&](const pb::Descriptor* d) {
        if (!done.insert(d).second) return;
        for (int i = 0; i < d->nested_type_count(); ++i) visit(d->nested_type(i));
        for (int i = 0; i < d->field_count(); ++i) {
          const pb::FieldDescriptor* f = d->field(i);
          if (f->type() == pb::FieldDescriptor::TYPE_MESSAGE &&
              f->message_type()->file() == file) {
            visit(f->message_type());
          }
        }
        out.push_back(d);
      };
  for (int i = 0; i < file->message_type_count(); ++i) {
    visit(file->message_type(i));
  }
}

void collectEnums(const pb::FileDescriptor* file,
                  std::vector<const pb::EnumDescriptor*>& out) {
  for (int i = 0; i < file->enum_type_count(); ++i) out.push_back(file->enum_type(i));
  std::vector<const pb::Descriptor*> stack;
  for (int i = 0; i < file->message_type_count(); ++i) stack.push_back(file->message_type(i));
  while (!stack.empty()) {
    const pb::Descriptor* d = stack.back();
    stack.pop_back();
    for (int i = 0; i < d->enum_type_count(); ++i) out.push_back(d->enum_type(i));
    for (int i = 0; i < d->nested_type_count(); ++i) stack.push_back(d->nested_type(i));
  }
}

void emitEnum(Writer& w, const pb::EnumDescriptor* e) {
  std::string name = sunEnumName(e);
  // Sun enums are dense i32 tags; proto values are arbitrary. Keep the Sun
  // enum symbolic and convert through explicit tables so unknown wire values
  // (open enums) still round-trip: an unrecognized value maps to the enum's
  // first variant on decode... except we preserve it: the class stores the
  // Sun enum, and unknown values are kept as the zero variant.
  std::string decl = "enum " + name + " {";
  w.open(decl);
  for (int i = 0; i < e->value_count(); ++i) {
    std::string sep = (i + 1 < e->value_count()) ? "," : "";
    w.line(e->value(i)->name() + sep);
  }
  w.close();
  w.line();

  // enum -> wire number
  w.open("function proto_enum_to_i32_" + name + "(v: " + name + ") i32 {");
  w.open("return match v {");
  for (int i = 0; i < e->value_count(); ++i) {
    std::string sep = (i + 1 < e->value_count()) ? "," : "";
    w.line(name + "." + e->value(i)->name() + " => " +
           std::to_string(e->value(i)->number()) + sep);
  }
  w.close("};");
  w.close();
  w.line();

  // wire number -> enum (unknown numbers map to the zero value)
  w.open("function proto_enum_from_i32_" + name + "(v: i32) " + name + " {");
  for (int i = 0; i < e->value_count(); ++i) {
    w.line("if (v == " + std::to_string(e->value(i)->number()) + ") { return " +
           name + "." + e->value(i)->name() + "; }");
  }
  const pb::EnumValueDescriptor* zero = e->value(0);
  for (int i = 0; i < e->value_count(); ++i) {
    if (e->value(i)->number() == 0) zero = e->value(i);
  }
  w.line("return " + name + "." + zero->name() + ";");
  w.close();
  w.line();
}

void emitMessage(Writer& w, const pb::Descriptor* d) {
  std::string name = sunTypeName(d);

  // ---- oneof enums: <Msg>_<oneof> { NotSet, FieldA(T), FieldB(U) } ----
  for (int i = 0; i < d->real_oneof_decl_count(); ++i) {
    const pb::OneofDescriptor* o = d->oneof_decl(i);
    w.open("enum " + oneofEnumName(o) + " {");
    w.line("NotSet,");
    for (int j = 0; j < o->field_count(); ++j) {
      const pb::FieldDescriptor* f = o->field(j);
      std::string sep = (j + 1 < o->field_count()) ? "," : "";
      w.line(oneofVariantName(f) + "(" + scalarSunType(f) + ")" + sep);
    }
    w.close();
    w.line();
  }

  // ---- class ----
  w.open("class " + name + " {");
  for (int i = 0; i < d->field_count(); ++i) {
    const pb::FieldDescriptor* f = d->field(i);
    if (f->real_containing_oneof()) continue;  // stored in the oneof field
    w.line("var " + f->name() + ": " + fieldSunType(f) + ";");
  }
  for (int i = 0; i < d->real_oneof_decl_count(); ++i) {
    const pb::OneofDescriptor* o = d->oneof_decl(i);
    w.line("var " + o->name() + ": " + oneofEnumName(o) + ";");
  }
  w.line("var unknown_fields: Vec<u8>;");
  w.line("var alloc_: HeapAllocator;");
  w.line();

  // init: zero values
  w.open("function init(alloc: ref HeapAllocator) {");
  w.line("this.alloc_ = alloc.copy();");
  for (int i = 0; i < d->field_count(); ++i) {
    const pb::FieldDescriptor* f = d->field(i);
    if (f->real_containing_oneof()) continue;
    if (f->is_map()) {
      w.line("this." + f->name() + " = " + fieldSunType(f) + "(alloc, 8);");
    } else if (f->is_repeated()) {
      w.line("this." + f->name() + " = " + fieldSunType(f) + "(alloc, 4);");
    } else if (f->has_optional_keyword()) {
      w.line("this." + f->name() + " = Option.None;");
    } else {
      w.line("this." + f->name() + " = " + zeroValue(f) + ";");
    }
  }
  for (int i = 0; i < d->real_oneof_decl_count(); ++i) {
    const pb::OneofDescriptor* o = d->oneof_decl(i);
    w.line("this." + o->name() + " = " + oneofEnumName(o) + ".NotSet;");
  }
  w.line("this.unknown_fields = Vec<u8>(alloc, 4);");
  w.close();
  w.line();

  // encode: append wire bytes
  w.open("function encode(buf: ref Vec<u8>) void {");
  for (int i = 0; i < d->field_count(); ++i) {
    const pb::FieldDescriptor* f = d->field(i);
    if (f->real_containing_oneof()) continue;  // emitted per oneof below
    std::string fld = "this." + f->name();
    std::string tag = std::to_string(f->number());
    if (f->is_map()) {
      // Each entry is a length-delimited message { 1: key, 2: value }
      const pb::Descriptor* entry = f->message_type();
      const pb::FieldDescriptor* k = entry->map_key();
      const pb::FieldDescriptor* v = entry->map_value();
      w.open("for (var i: i64 = 0; i < " + fld + ".capacity(); i = i + 1) {");
      w.open("if (" + fld + ".bucket_occupied(i)) {");
      w.line("var body = Vec<u8>(this.alloc_, 16);");
      w.line("proto_write_tag(body, 1, " + std::to_string(wireType(k)) + ");");
      w.line(writeStmt(k, fld + ".bucket_key(i)", "body"));
      w.line("proto_write_tag(body, 2, " + std::to_string(wireType(v)) + ");");
      w.line(writeStmt(v, fld + ".bucket_value(i)", "body"));
      w.line("proto_write_tag(buf, " + tag + ", 2);");
      w.line("proto_write_bytes(buf, body);");
      w.close();
      w.close();
    } else if (f->has_optional_keyword()) {
      // Explicit presence: written whenever set, even if zero
      w.open("match " + fld + " {");
      w.open("Option.Some(v) => {");
      w.line("proto_write_tag(buf, " + tag + ", " +
             std::to_string(wireType(f)) + ");");
      w.line(writeStmt(f, "v"));
      w.close("},");
      w.line("Option.None => { }");
      w.close("};");
    } else if (f->is_repeated()) {
      if (isScalarNumeric(f)) {
        // packed: one length-delimited record holding all elements
        w.open("if (" + fld + ".size() > 0) {");
        w.line("var body = Vec<u8>(this.alloc_, 16);");
        w.open("for (var i: i64 = 0; i < " + fld + ".size(); i = i + 1) {");
        // Elements go into the scratch body buffer, not `buf`
        w.line(writeStmt(f, fld + ".get_unchecked(i)", "body"));
        w.close();
        w.line("proto_write_tag(buf, " + tag + ", 2);");
        w.line("proto_write_bytes(buf, body);");
        w.close();
      } else {
        w.open("for (var i: i64 = 0; i < " + fld + ".size(); i = i + 1) {");
        w.line("proto_write_tag(buf, " + tag + ", " +
               std::to_string(wireType(f)) + ");");
        w.line(writeStmt(f, fld + ".get_unchecked(i)"));
        w.close();
      }
    } else {
      w.open("if (" + nonZeroTest(f, fld) + ") {");
      w.line("proto_write_tag(buf, " + tag + ", " +
             std::to_string(wireType(f)) + ");");
      w.line(writeStmt(f, fld));
      w.close();
    }
  }
  // oneofs: whichever variant is set is written
  for (int i = 0; i < d->real_oneof_decl_count(); ++i) {
    const pb::OneofDescriptor* o = d->oneof_decl(i);
    std::string en = oneofEnumName(o);
    w.open("match this." + o->name() + " {");
    for (int j = 0; j < o->field_count(); ++j) {
      const pb::FieldDescriptor* f = o->field(j);
      w.open(en + "." + oneofVariantName(f) + "(v) => {");
      w.line("proto_write_tag(buf, " + std::to_string(f->number()) + ", " +
             std::to_string(wireType(f)) + ");");
      w.line(writeStmt(f, "v"));
      w.close("},");
    }
    w.line(en + ".NotSet => { }");
    w.close("};");
  }
  w.line("proto_append_raw(buf, this.unknown_fields);");
  w.close();
  w.line();

  // encode_nested: length-prefixed (used when embedded in another message)
  w.open("function encode_nested(buf: ref Vec<u8>) void {");
  w.line("var body = Vec<u8>(this.alloc_, 16);");
  w.line("this.encode(body);");
  w.line("proto_write_bytes(buf, body);");
  w.close();
  w.line();

  // encode_delimited: varint length prefix for stream framing
  w.open("function encode_delimited(buf: ref Vec<u8>) void {");
  w.line("this.encode_nested(buf);");
  w.close();

  w.close();  // class
  w.line();

  // ---- decode (free functions) ----
  w.open("function " + name +
         "_decode_from(alloc: ref HeapAllocator, r: ref ProtoReader) " + name +
         ", IError {");
  w.line("var msg = " + name + "(alloc);");
  w.open("while (r.at_end() == false) {");
  w.line("var tag: u64 = r.read_tag();");
  w.line("var field: i64 = proto_tag_field(tag);");
  w.line("var wire: i64 = proto_tag_wire_type(tag);");
  bool first = true;
  for (int i = 0; i < d->field_count(); ++i) {
    const pb::FieldDescriptor* f = d->field(i);
    std::string fld = "msg." + f->name();
    std::string cond = (first ? "if (" : "} else if (") + std::string("field == ") +
                       std::to_string(f->number()) + ") {";
    first = false;
    w.open(cond);
    if (f->real_containing_oneof()) {
      const pb::OneofDescriptor* o = f->real_containing_oneof();
      w.line("msg." + o->name() + " = " + oneofEnumName(o) + "." +
             oneofVariantName(f) + "(" + readExpr(f) + ");");
    } else if (f->is_map()) {
      const pb::Descriptor* entry = f->message_type();
      const pb::FieldDescriptor* k = entry->map_key();
      const pb::FieldDescriptor* v = entry->map_value();
      w.line("var end: i64 = r.read_length();");
      w.line("var old: i64 = r.push_limit(end);");
      w.line("var key: " + scalarSunType(k) + " = " + zeroValue(k) + ";");
      w.line("var val: " + scalarSunType(v) + " = " + zeroValue(v) + ";");
      w.open("while (r.at_end() == false) {");
      w.line("var etag: u64 = r.read_tag();");
      w.line("var efield: i64 = proto_tag_field(etag);");
      w.open("if (efield == 1) {");
      w.line("key = " + readExpr(k) + ";");
      w.close("} else if (efield == 2) {");
      w.open("");
      w.line("val = " + readExpr(v) + ";");
      w.close("} else {");
      w.open("");
      w.line("r.skip_field(proto_tag_wire_type(etag), etag, msg.unknown_fields);");
      w.close();
      w.close();
      w.line("r.pop_limit(old);");
      w.line(fld + ".insert(key, val);");
    } else if (f->has_optional_keyword()) {
      w.line(fld + " = Option.Some(" + readExpr(f) + ");");
    } else if (f->is_repeated()) {
      if (isScalarNumeric(f)) {
        // Accept both packed (wire 2) and unpacked encodings
        w.open("if (wire == 2) {");
        w.line("var end: i64 = r.read_length();");
        w.line("var old: i64 = r.push_limit(end);");
        w.open("while (r.at_end() == false) {");
        w.line(fld + ".push(" + readExpr(f) + ");");
        w.close();
        w.line("r.pop_limit(old);");
        w.close("} else {");
        w.open("");
        w.line(fld + ".push(" + readExpr(f) + ");");
        w.close();
      } else {
        w.line(fld + ".push(" + readExpr(f) + ");");
      }
    } else {
      w.line(fld + " = " + readExpr(f) + ";");
    }
    w.closeSilently();
  }
  if (!first) {
    w.open("} else {");
  } else {
    w.open("if (true) {");
  }
  w.line("r.skip_field(wire, tag, msg.unknown_fields);");
  w.close();
  w.close();  // while
  w.line("return msg;");
  w.close();
  w.line();

  // Decode a length-prefixed nested message
  w.open("function " + name +
         "_decode_nested(alloc: ref HeapAllocator, r: ref ProtoReader) " +
         name + ", IError {");
  w.line("var end: i64 = r.read_length();");
  w.line("var old: i64 = r.push_limit(end);");
  w.line("var msg = " + name + "_decode_from(alloc, r);");
  w.line("r.pop_limit(old);");
  w.line("return msg;");
  w.close();
  w.line();

  // Decode from a whole buffer
  w.open("function " + name +
         "_decode(alloc: ref HeapAllocator, buf: ref Vec<u8>) " + name +
         ", IError {");
  w.line("var r = ProtoReader(buf);");
  w.line("return " + name + "_decode_from(alloc, r);");
  w.close();
  w.line();

  // Decode one delimited (length-prefixed) message from a reader
  w.open("function " + name +
         "_decode_delimited(alloc: ref HeapAllocator, r: ref ProtoReader) " +
         name + ", IError {");
  w.line("return " + name + "_decode_nested(alloc, r);");
  w.close();
  w.line();
}

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

}  // namespace

namespace {

// Emit one .proto file's module into `w`
void emitFile(Writer& w, const pb::FileDescriptor* file,
              const std::string& displayPath, const std::string& sourceText) {
  rejectUnsupported(file);
  rejectRecursiveMessages(file);

  w.line("// generated by sun proto-import v1 from " + displayPath);
  w.line("// sha256: " + sha256Hex(sourceText));
  w.line();

  std::string pkg = file->package();
  bool hasPkg = !pkg.empty();
  if (hasPkg) w.open("module " + pkg + " {");
  // Inside the module: merged ASTs order modules before other top-level
  // statements, so a file-level `using` would bind too late for the shapes.
  // Imported packages are brought into scope the same way.
  w.line("using sun;");
  std::set<std::string> importedPkgs;
  for (int i = 0; i < file->dependency_count(); ++i) {
    const std::string& dpkg = file->dependency(i)->package();
    if (!dpkg.empty() && dpkg != pkg && importedPkgs.insert(dpkg).second) {
      w.line("using " + dpkg + ";");
    }
  }
  w.line();

  std::vector<const pb::EnumDescriptor*> enums;
  collectEnums(file, enums);
  for (const auto* e : enums) emitEnum(w, e);

  std::vector<const pb::Descriptor*> messages;
  orderMessages(file, messages);
  for (const auto* d : messages) emitMessage(w, d);

  if (hasPkg) w.close();
  w.line();
}

std::string readFile(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

SynthesizedProtoModule ProtoImporter::import(
    const std::string& protoPath, const std::vector<std::string>& importDirs) {
  namespace fs = std::filesystem;
  fs::path path = fs::absolute(protoPath);
  if (!fs::exists(path)) {
    throw SunError(SunError::Kind::Compile,
                   "proto import: file not found: " + protoPath);
  }

  // Map every import dir (and the proto's own directory) onto the virtual
  // root so proto-level `import "x.proto"` resolves like other Sun imports.
  pbc::DiskSourceTree tree;
  std::vector<std::string> dirs = importDirs;
  dirs.insert(dirs.begin(), path.parent_path().string());
  for (const auto& d : dirs) tree.MapPath("", d);

  // The file itself is addressed by its name relative to its own directory
  std::string virtualName = path.filename().string();

  ErrorCollector errors;
  pbc::Importer importer(&tree, &errors);
  const pb::FileDescriptor* file = importer.Import(virtualName);
  if (!file || errors.hasErrors()) {
    throw SunError(SunError::Kind::Compile,
                   "proto import: " + errors.joined());
  }

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
