// package.cpp — verified dependency directories and reproducible distributions.
#include "driver/package.h"

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>

#include "driver/manifest_processor.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_cache.h"
#include "support/error.h"
#include "support/target_os.h"

/** Downloads configured inputs and assembles distributable artifacts. */
namespace sun::driver {
/** Keeps archive implementation details local. */
namespace {
/** Shortens filesystem operations on package paths. */
namespace fs = std::filesystem;
/** Reports dependency and packaging failures through compiler diagnostics. */
[[noreturn]] void fail(const std::string& message) {
  sun::support::logAndThrowError("package: " + message);
}

/** Reads complete file contents or reports an input error. */
std::string read(const fs::path& path) {
  auto buffer = llvm::MemoryBuffer::getFile(path.string());
  if (!buffer) fail("cannot read " + path.string());
  return (*buffer)->getBuffer().str();
}

/** Writes bytes and reports incomplete output. */
void write(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(contents.data(), contents.size());
  out.close();
  if (!out) fail("cannot write " + path.string());
}

/** Owns a unique temporary directory and removes it on every exit path. */
struct Temporary {
  fs::path path;
  /** Creates a private temporary directory beside the eventual output. */
  explicit Temporary(const fs::path& parent) {
    fs::create_directories(parent);
    std::string pattern = (parent / ".sun-package-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
      fail("cannot create temporary directory in " + parent.string());
    path = pattern;
  }
  /** Removes unpublished temporary files without throwing. */
  ~Temporary() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

/** Serializes writers to one dependency or distribution across processes. */
class FileLock {
  int descriptor_;

 public:
  /** Opens a private lock file and waits for exclusive access. */
  explicit FileLock(const fs::path& path) {
    descriptor_ =
        open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor_ < 0) fail("cannot open lock " + path.string());
    if (flock(descriptor_, LOCK_EX) != 0) {
      close(descriptor_);
      fail("cannot lock " + path.string());
    }
  }
  /** Releases the lock when work or error recovery finishes. */
  ~FileLock() { close(descriptor_); }
};

/** Rejects paths that could escape an archive root or differ across platforms.
 */
std::string safePath(const std::string& name) {
  fs::path path(name);
  if (name.empty() || path.is_absolute() ||
      name.find('\\') != std::string::npos ||
      name.find(':') != std::string::npos ||
      name.find('\0') != std::string::npos)
    fail("unsafe archive path '" + name + "'");
  for (const auto& part : path)
    if (part == ".." || part == ".") fail("unsafe archive path '" + name + "'");
  auto normalized = path.lexically_normal().generic_string();
  while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();
  if (normalized.empty() || normalized == ".") fail("empty archive path");
  return normalized;
}

/** Validates payload destinations and reserves the package metadata path. */
std::string payloadPath(const std::string& path) {
  auto name = safePath(path);
  if (name == "package.json" || name.rfind("package.json/", 0) == 0)
    fail("package.json is reserved");
  return name;
}

/** Checks whether a path is at or beneath another canonical location. */
bool beneath(const fs::path& path, const fs::path& parent) {
  auto p = fs::weakly_canonical(path);
  auto root = fs::weakly_canonical(parent);
  auto a = p.begin();
  for (auto b = root.begin(); b != root.end(); ++b, ++a)
    if (a == p.end() || *a != *b) return false;
  return true;
}

/** Rejects symlinks in a resource path, including its parent components. */
void noLinks(const fs::path& path) {
  fs::path current;
  for (const auto& part : fs::absolute(path)) {
    current /= part;
    if (fs::is_symlink(fs::symlink_status(current)))
      fail("symlink input " + current.string());
  }
}

/** Returns the normalized executable bit carried by a regular file. */
bool executable(const fs::path& path) {
  return (fs::status(path).permissions() &
          (fs::perms::owner_exec | fs::perms::group_exec |
           fs::perms::others_exec)) != fs::perms::none;
}

/** Sets the portable permissions retained in distributions. */
void permissions(const fs::path& path, bool exec) {
  fs::permissions(path, exec ? fs::perms(0755) : fs::perms(0644));
}

/** A normalized archive payload entry, with its source identity. */
struct Payload {
  fs::path source;
  std::string hash;
  bool exec = false;
  bool artifact = false;
};
/** Maps archive paths to their unique payload entries in sorted order. */
using Inventory = std::map<std::string, Payload>;

/** Checks archive library errors while preserving useful diagnostics. */
void archiveCheck(int status, archive* handle) {
  if (status < ARCHIVE_OK) {
    const char* message = archive_error_string(handle);
    fail(message ? message : "archive operation failed");
  }
}

/** Extracts regular files and directories without following archive links. */
void extract(const fs::path& input, const fs::path& output) {
  std::unique_ptr<archive, decltype(&archive_read_free)> reader(
      archive_read_new(), archive_read_free);
  archive_read_support_filter_gzip(reader.get());
  archive_read_support_format_tar(reader.get());
  archiveCheck(archive_read_open_filename(reader.get(), input.c_str(), 65536),
               reader.get());
  std::set<std::string> seen;
  archive_entry* entry = nullptr;
  int status;
  while ((status = archive_read_next_header(reader.get(), &entry)) !=
         ARCHIVE_EOF) {
    archiveCheck(status, reader.get());
    const char* raw = archive_entry_pathname(entry);
    if (!raw) fail("archive entry has no path");
    const auto name = safePath(raw);
    if (!seen.insert(name).second) fail("duplicate archive entry " + name);
    if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
      fail("archive links are unsupported");
    const auto type = archive_entry_filetype(entry);
    const auto dest = output / name;
    if (type == AE_IFDIR) {
      fs::create_directories(dest);
      continue;
    }
    if (type != AE_IFREG) fail("archive special files are unsupported");
    fs::create_directories(dest.parent_path());
    std::ofstream out(dest, std::ios::binary);
    std::array<char, 65536> buffer;
    la_ssize_t count;
    while ((count = archive_read_data(reader.get(), buffer.data(),
                                      buffer.size())) > 0)
      out.write(buffer.data(), count);
    if (count < 0) archiveCheck(static_cast<int>(count), reader.get());
    out.close();
    if (!out) fail("cannot extract " + name);
    permissions(dest, (archive_entry_perm(entry) & 0111) != 0);
  }
  archiveCheck(archive_read_close(reader.get()), reader.get());
}

/** Validates metadata and every payload against a freshly extracted archive. */
void validatePackage(const fs::path& root, const std::string& target) {
  auto parsed = llvm::json::parse(read(root / "package.json"));
  if (!parsed)
    fail("invalid package.json: " + llvm::toString(parsed.takeError()));
  const auto* obj = parsed->getAsObject();
  if (!obj || obj->getInteger("format_version") != 1 ||
      !obj->getString("name") || !obj->getString("target") ||
      !obj->getArray("files") || !obj->getArray("artifacts"))
    fail("unsupported or incomplete package metadata");
  if (configTargetKey(obj->getString("target")->str()) !=
      configTargetKey(target))
    fail("package target does not match " + target);
  std::set<std::string> files;
  for (const auto& value : *obj->getArray("files")) {
    const auto* file = value.getAsObject();
    if (!file || !file->getString("path") || !file->getString("hash") ||
        !file->getBoolean("executable"))
      fail("invalid package file inventory");
    const auto name = safePath(file->getString("path")->str());
    if (name == "package.json" || !files.insert(name).second)
      fail("duplicate or reserved inventory path");
    const auto path = root / name;
    if (!fs::is_regular_file(path) ||
        packageFileHash(path) != *file->getString("hash") ||
        executable(path) != *file->getBoolean("executable"))
      fail("package inventory mismatch for " + name);
  }
  for (const auto& item : fs::recursive_directory_iterator(root))
    if (item.is_regular_file() && item.path() != root / "package.json" &&
        !files.count(item.path().lexically_relative(root).generic_string()))
      fail("unlisted package payload " + item.path().string());
  std::set<std::string> names;
  for (const auto& value : *obj->getArray("artifacts")) {
    const auto* artifact = value.getAsObject();
    if (!artifact || !artifact->getString("name") ||
        !artifact->getString("kind") || !artifact->getString("path"))
      fail("invalid package artifact");
    auto name = artifact->getString("name")->str();
    auto kind = *artifact->getString("kind");
    auto path = safePath(artifact->getString("path")->str());
    if (!names.insert(name).second || !files.count(path) ||
        (kind != "library" && kind != "binary"))
      fail("invalid package artifact " + name);
    if (kind == "library" && fs::path(path).extension() != ".moon")
      fail("library artifact must name a .moon file");
  }
}

/** Returns whether two directories have identical files, bytes, and
 * permissions. */
bool sameTree(const fs::path& a, const fs::path& b) {
  if (!fs::is_directory(b) || fs::is_symlink(b)) return false;
  std::set<std::string> left, right;
  for (const auto& item : fs::recursive_directory_iterator(a)) {
    if (item.is_regular_file())
      left.insert(item.path().lexically_relative(a).generic_string());
  }
  for (const auto& item : fs::recursive_directory_iterator(b)) {
    if (item.is_symlink() || (!item.is_regular_file() && !item.is_directory()))
      return false;
    if (item.is_regular_file())
      right.insert(item.path().lexically_relative(b).generic_string());
  }
  if (left != right) return false;
  for (const auto& name : left)
    if (packageFileHash(a / name) != packageFileHash(b / name) ||
        executable(a / name) != executable(b / name))
      return false;
  return true;
}

/** Replaces a directory while preserving the old contents if publication fails.
 */
void publishDirectory(const fs::path& source, const fs::path& dest,
                      const fs::path& backup) {
  const bool existed = fs::exists(dest) || fs::is_symlink(dest);
  if (existed) fs::rename(dest, backup);
  try {
    fs::rename(source, dest);
  } catch (...) {
    if (existed) fs::rename(backup, dest);
    throw;
  }
}

/** Adds a payload file, rejecting ambiguous output mappings. */
void addFile(Inventory& files, const fs::path& source,
             const std::string& destination, bool artifact = false,
             bool forceExecutable = false) {
  const auto name = payloadPath(destination);
  noLinks(source);
  if (!fs::is_regular_file(source))
    fail("expected regular file " + source.string());
  Payload file{source, packageFileHash(source),
               forceExecutable || executable(source), artifact};
  if (auto it = files.find(name); it != files.end()) {
    if (artifact || it->second.artifact ||
        fs::weakly_canonical(it->second.source) != fs::weakly_canonical(source))
      fail("conflicting destination " + name);
    return;
  }
  for (const auto& [existing, value] : files)
    if (name.rfind(existing + "/", 0) == 0 ||
        existing.rfind(name + "/", 0) == 0)
      fail("file/directory destination conflict " + name);
  files.emplace(name, std::move(file));
}

/** Writes a stable JSON document used as both inventory and packaging
 * fingerprint. */
std::string metadata(const PackagePlan& plan, const std::string& target,
                     const Inventory& files, llvm::json::Array artifacts) {
  llvm::json::Array inventory;
  for (const auto& [path, file] : files)
    inventory.push_back(llvm::json::Object{
        {"path", path}, {"hash", file.hash}, {"executable", file.exec}});
  llvm::json::Value value(
      llvm::json::Object{{"format_version", 1},
                         {"name", plan.name},
                         {"target", configTargetKey(target)},
                         {"artifacts", std::move(artifacts)},
                         {"files", std::move(inventory)}});
  std::string result;
  llvm::raw_string_ostream out(result);
  out << value;
  return result;
}

/** Writes a deterministic gzip-compressed tar from the staged payload. */
void compress(const fs::path& root, const fs::path& output) {
  std::unique_ptr<archive, decltype(&archive_write_free)> writer(
      archive_write_new(), archive_write_free);
  archiveCheck(archive_write_set_format_pax_restricted(writer.get()),
               writer.get());
  archiveCheck(archive_write_add_filter_gzip(writer.get()), writer.get());
  archiveCheck(archive_write_set_filter_option(writer.get(), "gzip",
                                               "timestamp", nullptr),
               writer.get());
  archiveCheck(archive_write_open_filename(writer.get(), output.c_str()),
               writer.get());
  std::map<std::string, fs::path> sorted;
  for (const auto& item : fs::recursive_directory_iterator(root))
    if (item.is_regular_file())
      sorted[item.path().lexically_relative(root).generic_string()] =
          item.path();
  for (const auto& [name, path] : sorted) {
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), name.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), executable(path) ? 0755 : 0644);
    archive_entry_set_uid(entry.get(), 0);
    archive_entry_set_gid(entry.get(), 0);
    archive_entry_set_mtime(entry.get(), 0, 0);
    archive_entry_set_size(entry.get(), fs::file_size(path));
    archiveCheck(archive_write_header(writer.get(), entry.get()), writer.get());
    std::ifstream in(path, std::ios::binary);
    std::array<char, 65536> buffer;
    while (in) {
      in.read(buffer.data(), buffer.size());
      const auto size = in.gcount();
      if (size && archive_write_data(writer.get(), buffer.data(), size) != size)
        fail("cannot write archive data");
    }
    if (!in.eof()) fail("cannot read staged file " + path.string());
  }
  archiveCheck(archive_write_close(writer.get()), writer.get());
}
}  // namespace

std::string packageFileHash(const fs::path& path) {
  return sun::moon_bundling::computeSha256Hex(read(path));
}

fs::path resolveConfigDependency(const SunConfig& config,
                                 const std::string& name) {
  const auto& dep = config.dependencies.at(name);
  if (!dep.available)
    fail("dependency " + name + " has no archive for " + config.targetTriple);
  fs::path cache;
  if (const char* env = std::getenv("SUN_DEPENDENCY_CACHE"))
    cache = env;
  else if (const char* home = std::getenv("HOME"))
    cache = fs::path(home) / ".sun/cache/dependencies";
  else
    cache = fs::temp_directory_path() / "sun-cache/dependencies";
  cache = fs::weakly_canonical(fs::absolute(cache));
  const auto parent = cache / name / configTargetKey(config.targetTriple);
  fs::create_directories(parent);
  noLinks(parent);
  FileLock lock(parent / ".lock");
  fs::path input = dep.path;
  if (!dep.url.empty())
    input = sun::moon_bundling::MoonCache::fetch(
        dep.url, dep.hash, cache / ".downloads" / dep.hash);
  noLinks(input);
  const auto hash = packageFileHash(input);
  if (!dep.hash.empty() && hash != dep.hash)
    fail("hash mismatch for dependency " + name);
  const auto dest = parent / hash;
  Temporary tmp(parent);
  const auto stage = tmp.path / "contents";
  fs::create_directory(stage);
  if (dep.package) {
    extract(input, stage);
    validatePackage(stage, config.targetTriple);
  } else {
    fs::copy_file(input, stage / dep.filename);
    permissions(stage / dep.filename, false);
    if (packageFileHash(stage / dep.filename) != hash)
      fail("dependency changed while copying " + name);
  }
  if (packageFileHash(input) != hash)
    fail("dependency changed while extracting " + name);
  if (!sameTree(stage, dest))
    publishDirectory(stage, dest, tmp.path / "previous");
  return dest;
}

std::vector<PackagePlan> planPackages(
    const SunConfig& config, const std::vector<PackageArtifact>& artifacts) {
  std::vector<PackagePlan> result;
  std::set<std::string> assigned;
  std::map<std::string, size_t> activeEntries;
  for (size_t i = 0; i < config.entrypoints.size(); ++i)
    if (!config.entrypoints[i].name.empty())
      activeEntries.emplace(config.entrypoints[i].name, i);
  for (const auto& pkg : config.packages) {
    PackagePlan plan;
    plan.name = pkg.name;
    plan.output = pkg.outputName;
    plan.resources = pkg.resources;
    std::set<std::string> members;
    for (const auto& name : pkg.entrypoints) {
      if (!members.insert(name).second) continue;
      const auto found = activeEntries.find(name);
      if (found == activeEntries.end()) continue;
      const auto index = found->second;
      const auto& entry = config.entrypoints[index];
      plan.artifacts.push_back(artifacts.at(index));
      plan.resources.insert(plan.resources.end(), entry.resources.begin(),
                            entry.resources.end());
      assigned.insert(name);
    }
    if (plan.artifacts.empty())
      fail("package " + pkg.name + " has no active entrypoints");
    result.push_back(std::move(plan));
  }
  for (size_t i = 0; i < config.entrypoints.size(); ++i) {
    const auto& entry = config.entrypoints[i];
    if (!entry.resources.empty() && !assigned.count(entry.name))
      result.push_back({entry.name, {}, {artifacts.at(i)}, entry.resources});
  }
  std::vector<fs::path> outputs;
  for (auto& plan : result) {
    if (plan.output.empty())
      plan.output = config.configDir / "dist" /
                    (plan.name + "-" + configTargetKey(config.targetTriple));
    plan.output = fs::absolute(
        ManifestProcessor::expandPathVariables(plan.output.string(), &config));
    for (const auto& path :
         {plan.output, fs::path(plan.output.string() + ".tar.gz")}) {
      for (const auto& previous : outputs)
        if (beneath(path, previous) || beneath(previous, path))
          fail("overlapping package outputs");
      outputs.push_back(path);
    }
    for (auto& resource : plan.resources) {
      resource.destination = payloadPath(resource.destination);
      resource.path =
          ManifestProcessor::expandPathVariables(resource.path, &config);
      noLinks(resource.path);
      if (!fs::exists(resource.path)) fail("missing resource " + resource.path);
    }
  }
  for (const auto& output : outputs) {
    for (const auto& plan : result)
      for (const auto& resource : plan.resources)
        if (beneath(output, resource.path) || beneath(resource.path, output))
          fail("package output overlaps resource input");
    for (const auto& artifact : artifacts)
      if (beneath(artifact.path, output) || beneath(output, artifact.path))
        fail("package output overlaps build artifact");
  }
  return result;
}

void buildPackages(const SunConfig& config,
                   const std::vector<PackagePlan>& plans) {
  const bool windows =
      sun::support::resolvedTargetTriple(config.targetTriple).isOSWindows();
  for (const auto& plan : plans) {
    fs::create_directories(plan.output.parent_path());
    noLinks(plan.output.parent_path());
    FileLock lock(plan.output.string() + ".lock");
    Inventory files;
    llvm::json::Array artifacts;
    for (const auto& artifact : plan.artifacts) {
      const auto path = (artifact.library ? "lib/" : "bin/") + artifact.name +
                        (artifact.library ? ".moon"
                         : windows        ? ".exe"
                                          : "");
      addFile(files, artifact.path, path, true, !artifact.library);
      artifacts.push_back(
          llvm::json::Object{{"name", artifact.name},
                             {"kind", artifact.library ? "library" : "binary"},
                             {"path", path}});
    }
    for (const auto& resource : plan.resources) {
      const fs::path source(resource.path);
      noLinks(source);
      if (fs::is_directory(source)) {
        for (const auto& item : fs::recursive_directory_iterator(source)) {
          if (item.is_symlink())
            fail("symlink resource " + item.path().string());
          if (item.is_directory()) continue;
          addFile(files, item.path(),
                  (fs::path(resource.destination) /
                   item.path().lexically_relative(source))
                      .generic_string());
        }
      } else
        addFile(files, source, resource.destination);
    }
    const auto description =
        metadata(plan, config.targetTriple, files, std::move(artifacts));
    Temporary tmp(plan.output.parent_path());
    auto stage = tmp.path / "contents";
    fs::create_directory(stage);
    for (const auto& [name, file] : files) {
      fs::create_directories((stage / name).parent_path());
      fs::copy_file(file.source, stage / name);
      permissions(stage / name, file.exec);
      if (packageFileHash(stage / name) != file.hash)
        fail("input changed while packaging " + file.source.string());
    }
    write(stage / "package.json", description);
    permissions(stage / "package.json", false);
    const auto archivePath = fs::path(plan.output.string() + ".tar.gz");
    bool unchanged = sameTree(stage, plan.output);
    if (unchanged && fs::is_regular_file(archivePath) &&
        !fs::is_symlink(archivePath)) {
      // Verify archive contents too; metadata alone cannot prove an intact
      // output.
      auto check = tmp.path / "check";
      fs::create_directory(check);
      try {
        extract(archivePath, check);
        unchanged = sameTree(stage, check);
      } catch (const sun::support::SunError&) {
        unchanged = false;
      }
      if (unchanged) continue;
    }
    const auto archiveTmp = tmp.path / "output.tar.gz";
    compress(stage, archiveTmp);
    publishDirectory(stage, plan.output, tmp.path / "previous");
    fs::rename(archiveTmp, archivePath);
  }
}
}  // namespace sun::driver
