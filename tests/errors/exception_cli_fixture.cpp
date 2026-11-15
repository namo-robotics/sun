/** Executes integration fixtures without shell quoting or process-wide
 * environment changes. */
#include "exception_cli_fixture.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>

extern char** environ;

/** Keeps child process resources local to the invoking test. */
namespace sun::tests::exceptions {
/** Closes file descriptors on every exit path. */
class FileDescriptor {
 public:
  int value;

  /** Takes ownership of a successfully opened descriptor. */
  explicit FileDescriptor(int descriptor) : value(descriptor) {
    if (value < 0) {
      throw std::runtime_error("Cannot open process capture descriptor");
    }
  }

  /** Releases the operating system resource. */
  ~FileDescriptor() { close(value); }

  /** Descriptors have one owner. */
  FileDescriptor(const FileDescriptor&) = delete;
  /** Descriptors cannot be reassigned. */
  FileDescriptor& operator=(const FileDescriptor&) = delete;
};

void CompilerFixture::SetUp() {
  std::filesystem::create_directories(root / "tmp");
  auto pattern = (root / "tmp" / "exception cli XXXXXX").string();
  auto* created = mkdtemp(pattern.data());
  ASSERT_NE(created, nullptr);
  directory = created;
}

void CompilerFixture::TearDown() { std::filesystem::remove_all(directory); }

std::string CompilerFixture::read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(input), {}};
}

std::string CompilerFixture::write(const std::string& name,
                                   const std::string& contents) {
  const auto path = directory / name;
  std::ofstream output(path);
  output << contents;
  if (!output) {
    throw std::runtime_error("Cannot write " + path.string());
  }
  return path.string();
}

std::string CompilerFixture::fixture(const std::string& name) {
  return write(name, read(root / "tests/errors/fixtures" / name));
}

ProcessResult CompilerFixture::run(std::vector<std::string> command,
                                   int expected, bool terminal) {
  const auto stem = directory / ("process-" + std::to_string(invocation_++));
  const auto out = stem.string() + ".out";
  const auto err = stem.string() + ".err";
  FileDescriptor output(
      open(out.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600));
  FileDescriptor errors(
      open(err.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600));
  FileDescriptor input(open("/dev/null", O_RDONLY | O_CLOEXEC));
  // PTYs are only needed for terminal formatting checks; ordinary children
  // write files.
  std::optional<FileDescriptor> master, slave;
  if (terminal) {
    master.emplace(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
    if (grantpt(master->value) || unlockpt(master->value)) {
      throw std::runtime_error("Cannot create terminal capture");
    }
    const auto* name = ptsname(master->value);
    if (!name) {
      throw std::runtime_error("Cannot find terminal slave");
    }
    slave.emplace(open(name, O_RDWR | O_NOCTTY | O_CLOEXEC));
  }
  std::vector<std::string> environment;
  for (auto** item = environ; *item; ++item) {
    std::string entry(*item);
    if (!entry.starts_with("TERM=") && !entry.starts_with("TERM_PROGRAM=")) {
      environment.push_back(std::move(entry));
    }
  }
  environment.emplace_back("TERM=xterm-256color");
  environment.emplace_back("TERM_PROGRAM=vscode");
  std::vector<char*> argv, envp;
  for (auto& argument : command) {
    argv.push_back(argument.data());
  }
  for (auto& entry : environment) {
    envp.push_back(entry.data());
  }
  argv.push_back(nullptr);
  envp.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, input.value, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(
      &actions, terminal ? slave->value : output.value, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(
      &actions, terminal ? slave->value : errors.value, STDERR_FILENO);
  if (terminal) {
    posix_spawn_file_actions_addclose(&actions, master->value);
    posix_spawn_file_actions_addclose(&actions, slave->value);
  }
  pid_t child;
  const int spawned = posix_spawnp(&child, argv[0], &actions, nullptr,
                                   argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  slave.reset();
  std::string captured;
  if (spawned == 0 && terminal) {
    char bytes[4096];
    for (;;) {
      const auto count = ::read(master->value, bytes, sizeof(bytes));
      if (count > 0) {
        captured.append(bytes, count);
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        break;  // A closed PTY reports EOF or EIO depending on the platform.
      }
    }
  }
  master.reset();
  if (spawned) {
    throw std::runtime_error("Cannot execute " + command.front());
  }
  int status;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      throw std::runtime_error("Cannot wait for child");
    }
  }
  ProcessResult result{
      WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
      terminal ? replace(captured, "\r\n", "\n") : read(out),
      terminal ? std::string{} : read(err)};
  EXPECT_EQ(result.status, expected) << testing::PrintToString(command) << "\n"
                                     << result.output << result.error;
  return result;
}

ProcessResult CompilerFixture::sun(std::vector<std::string> arguments,
                                   int expected) {
  arguments.insert(arguments.begin(), compiler);
  return run(std::move(arguments), expected);
}

std::string CompilerFixture::native(const std::string& source,
                                    std::vector<std::string> options) {
  const auto binary = (directory / "native").string();
  options.insert(options.end(), {"--dynamic", "-c", source, "-o", binary});
  sun(std::move(options));
  return binary;
}

std::string CompilerFixture::ir(const std::string& source,
                                std::vector<std::string> options) {
  const auto output = (directory / "program.ll").string();
  options.insert(options.end(), {"--emit-llvm", "-O0", source, "-o", output});
  sun(std::move(options));
  return read(output);
}

std::string CompilerFixture::replace(std::string text, const std::string& from,
                                     const std::string& to) {
  std::size_t offset = 0;
  while ((offset = text.find(from, offset)) != std::string::npos) {
    text.replace(offset, from.size(), to);
    offset += to.size();
  }
  return text;
}

std::size_t CompilerFixture::occurrences(const std::string& text,
                                         const std::string& part) {
  std::size_t count = 0, offset = 0;
  while ((offset = text.find(part, offset)) != std::string::npos) {
    ++count;
    offset += part.size();
  }
  return count;
}

void CompilerFixture::expectRuntimeCleanup() {
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    EXPECT_EQ(entry.path().filename().string().find(".runtime-"),
              std::string::npos)
        << entry.path();
  }
}
}  // namespace sun::tests::exceptions
