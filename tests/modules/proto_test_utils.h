// tests/proto_test_utils.h — Shared helpers for native-protobuf-import tests:
// a temp-dir project holding .proto schemas + a Sun entrypoint that imports
// them, moon building, and libprotobuf-side parsing for cross-validation.

#pragma once

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "driver/driver.h"
#include "driver/execution_utils.h"
#include "moon_bundling/moon_builder.h"
#include "moon_bundling/moon_import.h"

namespace proto_test {

namespace fs = std::filesystem;

// Read a whole file as bytes
inline std::string readBytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline void writeFile(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p);
  out << text;
}

// A throwaway project under the temp directory:
//   <dir>/schemas/<name>.proto   schemas
//   <dir>/main.sun               entrypoint with `manifest { protos: [...] }`
class ProtoProject {
 public:
  explicit ProtoProject(const std::string& name)
      : dir_(fs::temp_directory_path() / name) {
    initTestEnvironment();
    fs::create_directories(dir_ / "schemas");
  }

  const fs::path& dir() const { return dir_; }
  fs::path schemasDir() const { return dir_ / "schemas"; }
  fs::path file(const std::string& rel) const { return dir_ / rel; }

  ProtoProject& addSchema(const std::string& fileName,
                          const std::string& text) {
    writeFile(dir_ / "schemas" / fileName, text);
    schemas_.push_back("schemas/" + fileName);
    return *this;
  }

  // Sun source that follows the generated manifest block. `manifestProtos`
  // defaults to every schema added so far.
  ProtoProject& setProgram(const std::string& program,
                           std::vector<std::string> manifestProtos = {}) {
    if (manifestProtos.empty()) manifestProtos = schemas_;
    std::string manifest = "manifest { protos: [";
    for (size_t i = 0; i < manifestProtos.size(); ++i) {
      if (i) manifest += ", ";
      manifest += "\"" + manifestProtos[i] + "\"";
    }
    manifest += "] }\n";
    writeFile(dir_ / "main.sun", manifest + program);
    return *this;
  }

  // JIT the entrypoint (stdlib.moon preloaded) and return main()'s value
  sun::SunValue run(std::vector<sun::MoonImport> extraMoons = {}) const {
    auto driver = Driver::createForJIT("proto_test");
    auto imports = getStdlibMoonImports();
    imports.insert(imports.end(), extraMoons.begin(), extraMoons.end());
    driver->setMoonImports(imports);
    return driver->executeFile((dir_ / "main.sun").string(), 0, nullptr);
  }

  // Build a .moon whose entrypoint manifest lists the project's schemas
  // (mirrors `sun --emit-moon`); returns the bundle path
  fs::path buildMoon(const std::string& libName = "lib") const {
    fs::path entry = dir_ / (libName + ".sun");
    std::string manifest = "manifest { protos: [";
    for (size_t i = 0; i < schemas_.size(); ++i) {
      if (i) manifest += ", ";
      manifest += "\"" + schemas_[i] + "\"";
    }
    manifest += "] }\n";
    writeFile(entry, manifest);
    fs::path out = dir_ / (libName + ".moon");
    sun::MoonBuildOptions options;
    options.extraMoons = getStdlibMoonImports();
    sun::MoonBuilder::build(entry.string(), out, options);
    return out;
  }

 private:
  fs::path dir_;
  std::vector<std::string> schemas_;
};

// Write a schema + program, run it, return main()'s value (the common shape
// of most round-trip tests)
inline sun::SunValue runWithProto(const std::string& projectName,
                                  const std::string& proto,
                                  const std::string& program) {
  ProtoProject project(projectName);
  project.addSchema("t.proto", proto).setProgram(program);
  return project.run();
}

// libprotobuf's view of a schema directory: parse a schema with libprotoc and
// build DynamicMessage instances for cross-validating Sun's bytes
class LibprotobufSchema {
 public:
  explicit LibprotobufSchema(const fs::path& schemasDir) : importer_(nullptr) {
    tree_.MapPath("", schemasDir.string());
    importer_ = std::make_unique<google::protobuf::compiler::Importer>(
        &tree_, &errors_);
  }

  const google::protobuf::FileDescriptor* import(const std::string& file) {
    return importer_->Import(file);
  }

  const std::string& errors() const { return errors_.messages; }

  // A fresh message of `typeName` from `file`, parsed from `bytes`
  // (nullptr if the type is unknown or the bytes don't parse)
  std::unique_ptr<google::protobuf::Message> parse(const std::string& file,
                                                   const std::string& typeName,
                                                   const std::string& bytes) {
    const auto* fd = import(file);
    if (!fd) return nullptr;
    const auto* desc = fd->FindMessageTypeByName(typeName);
    if (!desc) return nullptr;
    std::unique_ptr<google::protobuf::Message> msg(
        factory_.GetPrototype(desc)->New());
    if (!msg->ParseFromString(bytes)) return nullptr;
    return msg;
  }

 private:
  struct Collector : google::protobuf::compiler::MultiFileErrorCollector {
    void AddError(const std::string&, int, int,
                  const std::string& m) override {
      messages += m;
    }
    std::string messages;
  };
  google::protobuf::compiler::DiskSourceTree tree_;
  Collector errors_;
  std::unique_ptr<google::protobuf::compiler::Importer> importer_;
  google::protobuf::DynamicMessageFactory factory_;
};

// Sun program prologue that dumps a Vec<u8> named `buf` to `outFile`
inline std::string dumpBufferProgramTail(const fs::path& outFile) {
  return "  var fd: i32 = unsafe { __file_open(\"" + outFile.string() +
         "\", 1); };\n"
         "  if (fd < 0) { return 1; }\n"
         "  unsafe { __write(fd, buf.raw_data(), buf.size()); };\n"
         "  unsafe { __file_close(fd); };\n"
         "  return 0;\n}\n";
}

}  // namespace proto_test
