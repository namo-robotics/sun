#include <clang-c/Index.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MathExtras.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>

#include "driver/c_bindings.h"
#include "parsing/lexer.h"
#include "support/error.h"

/** Generates checked Sun declarations from the target's C headers. */
namespace sun::driver {
/** Keeps Clang ownership and translation details private to bindgen. */
namespace {

/** Copies a Clang-owned string and releases its storage. */
std::string text(CXString value) {
  const char* data = clang_getCString(value);
  std::string result = data ? data : "";
  clang_disposeString(value);
  return result;
}

/** Escapes a string for a Sun string literal. */
std::string quote(const std::string& value) {
  return llvm::formatv("{0}", llvm::json::Value(value)).str();
}

/** Reports an unsupported declaration with its C header location. */
[[noreturn]] void failure(CXCursor cursor, const std::string& message) {
  CXFile file;
  unsigned line, column;
  clang_getSpellingLocation(clang_getCursorLocation(cursor), &file, &line,
                            &column, nullptr);
  sun::support::logAndThrowError(
      (file ? text(clang_getFileName(file)) : "<header>") + ":" +
      std::to_string(line) + ":" + std::to_string(column) + ": " + message);
}

/** Collects immediate children before translating declarations. */
std::vector<CXCursor> children(CXCursor cursor) {
  std::vector<CXCursor> result;
  clang_visitChildren(
      cursor,
      [](CXCursor child, CXCursor, CXClientData data) {
        static_cast<std::vector<CXCursor>*>(data)->push_back(child);
        return CXChildVisit_Continue;
      },
      &result);
  return result;
}

/** Validates C identifiers and escapes names reserved by Sun. */
std::string identifier(const std::string& name) {
  const auto letter = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (name.empty() || !letter(name.front()) ||
      !std::all_of(name.begin(), name.end(),
                   [&](char c) { return letter(c) || (c >= '0' && c <= '9'); }))
    sun::support::logAndThrowError("unsupported identifier " + quote(name));
  // Preserve contextual names alongside the lexer's reserved words.
  static const auto reserved = [] {
    std::set<std::string_view> words = {"alignment", "private", "method",
                                        "init",      "deinit",  "test"};
    for (const auto& [kind, pattern] : sun::parsing::tokenRegexes)
      if (const auto word = sun::parsing::getKeywordSpelling(kind))
        words.insert(*word);
    return words;
  }();
  return name.front() == '_' || reserved.contains(name) ? "c_" + name : name;
}

/** Translates selected declarations and dependencies without guessing layouts.
 */
class Generator {
  std::map<std::string, std::string> records;
  std::map<std::string, std::string> functions;
  std::map<std::string, std::string> names;

  /** Assigns a unique Sun name and rejects collisions after escaping. */
  std::string name(CXCursor cursor) {
    const auto result = identifier(text(clang_getCursorSpelling(cursor)));
    const auto usr = text(clang_getCursorUSR(cursor));
    const auto [previous, inserted] = names.emplace(result, usr);
    if (!inserted && previous->second != usr)
      failure(cursor, "Sun name collision for " + quote(result));
    return result;
  }

  /** Maps canonical storage types using the selected target's widths. */
  std::string typeName(CXType type, CXCursor cursor, bool allowVoid = false) {
    const auto canonical = clang_getCanonicalType(type);
    if (clang_Type_getAlignOf(type) != clang_Type_getAlignOf(canonical))
      failure(cursor, "typedef-specific alignment is unsupported");
    type = canonical;
    if (clang_isVolatileQualifiedType(type))
      failure(cursor, "volatile types are unsupported");
    switch (type.kind) {
      case CXType_Void:
        if (allowVoid) return "void";
        break;
      case CXType_Bool:
        return "bool";
      case CXType_Char_S:
      case CXType_SChar:
      case CXType_Short:
      case CXType_Int:
      case CXType_Long:
      case CXType_LongLong:
      case CXType_Char_U:
      case CXType_UChar:
      case CXType_UShort:
      case CXType_UInt:
      case CXType_ULong:
      case CXType_ULongLong: {
        const auto size = clang_Type_getSizeOf(type);
        if (size != 1 && size != 2 && size != 4 && size != 8)
          failure(cursor, "unsupported integer width: " + std::to_string(size));
        const bool isUnsigned =
            type.kind == CXType_Char_U || type.kind == CXType_UChar ||
            type.kind == CXType_UShort || type.kind == CXType_UInt ||
            type.kind == CXType_ULong || type.kind == CXType_ULongLong;
        return std::string(isUnsigned ? "u" : "i") + std::to_string(size * 8);
      }
      case CXType_Float:
        return "f32";
      case CXType_Double:
        return "f64";
      case CXType_Pointer: {
        const auto pointee = clang_getPointeeType(type);
        const auto target = clang_getCanonicalType(pointee).kind == CXType_Void
                                ? "u8"
                                : typeName(pointee, cursor);
        return "raw_ptr<" + target + ">";
      }
      case CXType_ConstantArray:
        return "array<" + typeName(clang_getArrayElementType(type), cursor) +
               ", " + std::to_string(clang_getArraySize(type)) + ">";
      case CXType_Record: {
        const auto definition =
            clang_getCursorDefinition(clang_getTypeDeclaration(type));
        if (clang_Cursor_isNull(definition))
          failure(cursor, "incomplete record types are unsupported");
        return record(definition);
      }
      default:
        break;
    }
    failure(cursor,
            "unsupported C type " + quote(text(clang_getTypeSpelling(type))));
  }

  /** Emits a natural record, optionally with increased aggregate alignment. */
  std::string record(CXCursor cursor) {
    const auto spelling = text(clang_getCursorSpelling(cursor));
    if (cursor.kind != CXCursor_StructDecl || spelling.empty() ||
        clang_Cursor_isAnonymous(cursor))
      failure(cursor, "only named C structs are supported");
    const auto recordName = name(cursor);
    if (records.count(recordName)) return recordName;
    records[recordName] = "";  // Permit self-referential pointers.
    std::vector<CXCursor> fields;
    for (auto child : children(cursor)) {
      if (clang_isAttribute(child.kind) && child.kind != CXCursor_AlignedAttr)
        failure(child, "unsupported record attribute");
      if (child.kind == CXCursor_FieldDecl) fields.push_back(child);
    }
    if (fields.empty()) failure(cursor, "empty structs are unsupported");
    int64_t offset = 0, fieldAlign = 1;
    std::vector<int64_t> offsets;
    std::string declarations;
    std::set<std::string> fieldNames;
    for (auto field : fields) {
      if (clang_Cursor_isBitField(field))
        failure(field, "bitfields are unsupported");
      for (auto child : children(field))
        if (clang_isAttribute(child.kind))
          failure(field, "member-specific attributes are unsupported");
      const auto fieldName = identifier(text(clang_getCursorSpelling(field)));
      if (!fieldNames.insert(fieldName).second)
        failure(field, "field name collision");
      const auto type = clang_getCursorType(field);
      const auto typeSpelling = typeName(type, field);
      const int64_t alignment = clang_Type_getAlignOf(type);
      const int64_t size = clang_Type_getSizeOf(type);
      if (alignment <= 0 || size < 0)
        failure(field, "field layout is unavailable");
      offset = llvm::alignTo(offset, alignment);
      if (clang_Cursor_getOffsetOfField(field) != offset * 8)
        failure(field, "packing or member alignment cannot be represented");
      offsets.push_back(offset);
      offset += size;
      fieldAlign = std::max(fieldAlign, alignment);
      declarations += "  public var " + fieldName + ": " + typeSpelling + ";\n";
    }
    const auto type = clang_getCursorType(cursor);
    const auto alignment = clang_Type_getAlignOf(type);
    const auto size = clang_Type_getSizeOf(type);
    if (alignment < fieldAlign || alignment > 65536 ||
        (alignment & (alignment - 1)))
      failure(cursor, "unsupported record alignment");
    if (size != static_cast<int64_t>(llvm::alignTo(offset, alignment)))
      failure(cursor, "record padding cannot be represented");
    auto source = "/** Mirrors the target C struct " + spelling +
                  ". */\npublic class " + recordName + " {\n";
    if (alignment > fieldAlign)
      source += "  alignment: " + std::to_string(alignment) + "\n";
    source += declarations + "}\n_assert_c_layout<" + recordName + ">(" +
              std::to_string(size) + ", " + std::to_string(alignment);
    for (auto fieldOffset : offsets)
      source += ", " + std::to_string(fieldOffset);
    records[recordName] = source + ");\n";
    return recordName;
  }

  /** Emits a nonvariadic C prototype using its exact link symbol. */
  void function(CXCursor cursor) {
    if (clang_Cursor_getStorageClass(cursor) == CX_SC_Static ||
        clang_isCursorDefinition(cursor))
      failure(cursor, "static and inline function definitions are unsupported");
    const auto type = clang_getCursorType(cursor);
    if (type.kind != CXType_FunctionProto)
      failure(cursor,
              "functions require a complete C prototype; use (void) for no "
              "arguments");
    if (clang_isFunctionTypeVariadic(type))
      failure(cursor, "variadic functions are unsupported");
    const auto convention = clang_getFunctionTypeCallingConv(type);
    if (convention != CXCallingConv_Default && convention != CXCallingConv_C)
      failure(cursor, "only the default C calling convention is supported");
    const auto functionName = name(cursor);
    const auto result =
        typeName(clang_getCursorResultType(cursor), cursor, true);
    std::string arguments;
    const auto canonical = clang_getCanonicalType(type);
    for (int i = 0; i < clang_Cursor_getNumArguments(cursor); ++i) {
      if (i) arguments += ", ";
      arguments += "arg" + std::to_string(i) + ": " +
                   typeName(clang_getArgType(canonical, i),
                            clang_Cursor_getArgument(cursor, i));
    }
    const auto spelling = text(clang_getCursorSpelling(cursor));
    auto source = "/** Calls the C function " + spelling +
                  "; its foreign contract requires unsafe. */\npublic extern "
                  "\"C\" function " +
                  functionName + "(" + arguments + ") " + result;
    auto symbol = text(clang_Cursor_getMangling(cursor));
    if (symbol.empty()) symbol = spelling;
    if (functionName != symbol) source += " as " + quote(symbol);
    source += ";\n";
    const auto [previous, inserted] = functions.emplace(functionName, source);
    if (!inserted && previous->second != source)
      failure(cursor, "conflicting declarations for " + functionName);
  }

 public:
  /** Selects requested declarations and emits reproducible source. */
  std::string generate(CXTranslationUnit unit, const CBindingOptions& options) {
    std::set<std::string> selected(options.allow.begin(), options.allow.end());
    std::set<std::string> found;
    for (auto cursor : children(clang_getTranslationUnitCursor(unit))) {
      const auto spelling = text(clang_getCursorSpelling(cursor));
      if (!selected.count(spelling)) continue;
      found.insert(spelling);
      switch (cursor.kind) {
        case CXCursor_StructDecl: {
          const auto definition = clang_getCursorDefinition(cursor);
          if (clang_Cursor_isNull(definition))
            failure(cursor, "incomplete struct");
          record(definition);
          break;
        }
        case CXCursor_FunctionDecl:
          function(cursor);
          break;
        case CXCursor_TypedefDecl:
          typeName(clang_getTypedefDeclUnderlyingType(cursor), cursor);
          break;
        default:
          failure(cursor, "unsupported selected declaration");
      }
    }
    std::string missing;
    for (const auto& item : selected)
      if (!found.count(item)) missing += (missing.empty() ? "" : ", ") + item;
    if (!missing.empty())
      sun::support::logAndThrowError("declarations not found: " + missing);
    auto body = "_assert_c_target(" + quote(options.target) + ");\n\n";
    for (const auto* declarations : {&records, &functions}) {
      bool first = true;
      for (const auto& [name, source] : *declarations) {
        if (!first) body += "\n";
        first = false;
        body += source;
      }
    }
    auto source =
        std::string(
            "/** Auto-generated by sun bindgen. Do not edit manually. */\n\n") +
        "/** C bindings for " + options.module +
        "; regenerate for each target and SDK. */\n" + "public module " +
        options.module + " {\n";
    std::istringstream lines(body);
    for (std::string line; std::getline(lines, line);)
      source += (line.empty() ? "" : "  ") + line + "\n";
    return source + "}\n";
  }
};

/** Atomically replaces changed output, leaving prior bindings intact on
 * failure. */
void writeOutput(const std::string& path, const std::string& source) {
  if (path == "-") {
    llvm::outs() << source;
    return;
  }
  const auto existing = llvm::MemoryBuffer::getFile(path);
  if (existing && (*existing)->getBuffer() == source) return;
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent);
  llvm::sys::fs::file_status status;
  auto mode = llvm::sys::fs::all_read | llvm::sys::fs::owner_write;
  if (!llvm::sys::fs::status(path, status) && llvm::sys::fs::exists(status))
    mode = status.permissions();
  auto file = llvm::sys::fs::TempFile::create(path + ".%%%%%%", mode);
  if (!file)
    sun::support::logAndThrowError("Cannot write bindings: " +
                                   llvm::toString(file.takeError()));
  const auto cleanup =
      llvm::make_scope_exit([&] { llvm::consumeError(file->discard()); });
  llvm::raw_fd_ostream output(file->FD, false);
  output << source;
  output.flush();
  if (const auto error = output.error()) {
    output.clear_error();
    sun::support::logAndThrowError("Cannot write bindings: " + error.message());
  }
  if (auto error = file->keep(path))
    sun::support::logAndThrowError("Cannot replace bindings: " +
                                   llvm::toString(std::move(error)));
}
}  // namespace

void generateCBinding(const CBindingOptions& options) {
  if (identifier(options.module) != options.module)
    sun::support::logAndThrowError(
        "module must be a nonreserved Sun identifier");
  std::vector<std::string> storage = {"-x", "c", "-std=c11"};
  for (const auto& flag : options.clangArgs) {
    if (flag.starts_with("-target") || flag.starts_with("--target") ||
        flag.starts_with("-x"))
      sun::support::logAndThrowError(
          "use --target; input language is always C");
    storage.push_back(flag);
  }
  storage.push_back("--target=" + options.target);
  std::vector<const char*> args;
  for (const auto& arg : storage) args.push_back(arg.c_str());
  /** Owns the Clang index for the duration of one generation request. */
  using IndexOwner = std::unique_ptr<void, decltype(&clang_disposeIndex)>;
  IndexOwner index(clang_createIndex(0, 0), clang_disposeIndex);
  if (!index) sun::support::logAndThrowError("Cannot create Clang index");
  CXTranslationUnit unit = nullptr;
  const auto parsed = clang_parseTranslationUnit2(
      index.get(), options.header.c_str(), args.data(), args.size(), nullptr, 0,
      0, &unit);
  const auto cleanup = llvm::make_scope_exit([&] {
    if (unit) clang_disposeTranslationUnit(unit);
  });
  if (parsed != CXError_Success)
    sun::support::logAndThrowError("Clang could not parse header " +
                                   options.header);
  std::string errors;
  for (unsigned i = 0; i < clang_getNumDiagnostics(unit); ++i) {
    const auto diagnostic = clang_getDiagnostic(unit, i);
    if (clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error)
      errors += text(clang_formatDiagnostic(
                    diagnostic, clang_defaultDiagnosticDisplayOptions())) +
                "\n";
    clang_disposeDiagnostic(diagnostic);
  }
  if (!errors.empty()) sun::support::logAndThrowError(errors);
  writeOutput(options.output, Generator().generate(unit, options));
}
}  // namespace sun::driver
