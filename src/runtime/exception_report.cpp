/** Writes exception diagnostics without allocating memory or changing
 * ownership. */
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "runtime/exception_sources.h"
#include "runtime/exception_trace.h"
#include "runtime/exceptions.h"

/** Keeps bounded formatting and signal protection private to reporting. */
namespace {
/** Refers to the payload and trace metadata preceding the address storage. */
using sun::runtime::ExceptionValueHeader;

/** Writes a bounded buffer, retrying interruptions and stopping on output
 * failure. */
bool writeBytes(const char* bytes, std::size_t size) noexcept {
  while (size) {
    const auto written = ::write(STDERR_FILENO, bytes, size);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    bytes += written;
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

/** Prevents a closed diagnostic pipe from terminating the program during
 * reporting. */
class DiagnosticSignals {
 public:
  /** Blocks SIGPIPE on this thread and remembers whether it was already
   * pending. */
  DiagnosticSignals() noexcept {
    sigemptyset(&blocked_);
    sigaddset(&blocked_, SIGPIPE);
    active_ = pthread_sigmask(SIG_BLOCK, &blocked_, &previous_) == 0;
    sigset_t pending;
    if (active_ && sigpending(&pending) == 0) {
      alreadyPending_ = sigismember(&pending, SIGPIPE) == 1;
    }
  }

  /** Consumes only a newly generated pending SIGPIPE and restores the caller's
   * mask. */
  ~DiagnosticSignals() {
    if (!active_) {
      return;
    }
    sigset_t pending;
    if (!alreadyPending_ && sigpending(&pending) == 0 &&
        sigismember(&pending, SIGPIPE) == 1) {
      int signal;
      sigwait(&blocked_, &signal);
    }
    pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
  }

  /** Reports whether it is safe to attempt output to a potentially closed pipe.
   */
  bool ready() const noexcept { return active_; }

 private:
  sigset_t blocked_, previous_;
  bool active_ = false;
  bool alreadyPending_ = true;
};

/** Serializes reports into one reserved workspace, with no per-thread
 * allocation. */
pthread_mutex_t reportMutex = PTHREAD_MUTEX_INITIALIZER;
char reportWorkspace[sun::runtime::reportWorkspaceSize];

/** Appends complete trace lines while reserving room for an explicit truncation
 * marker. */
class ReportBuffer {
 public:
  std::size_t used = 0;
  bool truncated = false;
  bool links = isatty(STDERR_FILENO) != 0;
  std::size_t depth = 0;
  bool throwFrame = false;
  const char* terminal = std::getenv("TERM_PROGRAM");
  bool editorLinks = terminal && std::strcmp(terminal, "vscode") == 0;

  /** Copies a bounded string and replaces terminal control bytes in source
   * data. */
  void append(const char* text,
              std::size_t limit = sun::runtime::reportWorkspaceSize,
              bool sanitize = false) noexcept {
    for (std::size_t i = 0; text[i] && i < limit; ++i) {
      if (used == sizeof(reportWorkspace) - 80) {
        truncated = true;
        return;
      }
      const auto byte = static_cast<unsigned char>(text[i]);
      reportWorkspace[used++] =
          sanitize && (byte < 32 || byte == 127) ? '?' : text[i];
    }
  }

  /** Formats an integer without a locale, stream, or temporary allocation. */
  void number(std::uint64_t value, unsigned base = 10) noexcept {
    char text[32];
    auto* end = text + sizeof(text);
    *--end = 0;
    do {
      *--end = "0123456789abcdef"[value % base];
      value /= base;
    } while (value);
    append(end);
  }

  /** Encodes a path for an OSC hyperlink without allowing escape-sequence
   * injection. */
  void uri(const char* path) noexcept {
    constexpr char hex[] = "0123456789ABCDEF";
    for (std::size_t i = 0; path[i] && !truncated; ++i) {
      const auto byte = static_cast<unsigned char>(path[i]);
      if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '/' || byte == '-' ||
          byte == '_' || byte == '.' || byte == '~') {
        char literal[] = {static_cast<char>(byte), 0};
        append(literal);
      } else {
        char escaped[] = {'%', hex[byte >> 4], hex[byte & 15], 0};
        append(escaped);
      }
    }
  }

  /** Indents each nested connector two spaces beyond its caller's connector. */
  void framePrefix() noexcept {
    append("  ");
    for (std::size_t level = 1; level < depth && !truncated; ++level) {
      append("  ");
    }
    if (depth++) {
      append("└─ ");
    }
  }

  /** Emits one linked source frame, rolling back incomplete escape sequences on
   * overflow. */
  void frame(const sun::runtime::ExceptionFrameView& frame, const char* label,
             bool call) noexcept {
    if (truncated) {
      return;
    }
    const auto start = used;
    const bool location = frame.file[0] && frame.line;
    const bool linked = links && location;
    framePrefix();
    if (linked) {
      const bool relative = frame.file[0] != '/';
      if (relative) {
        append("\033]8;;sun-source:ENTRYPOINT/");
      } else {
        append(editorLinks ? "\033]8;;vscode://file" : "\033]8;;file://");
      }
      uri(frame.file);
      append(relative || editorLinks ? ":" : "#L");
      number(frame.line);
      if (relative || editorLinks) {
        append(":");
        number(frame.column ? frame.column : 1);
      }
      append("\033\\");
    }
    append(label, sun::runtime::reportNameLimit, true);
    if (std::strlen(label) > sun::runtime::reportNameLimit) {
      append("...");
    }
    if (call) {
      append("()");
    }
    if (location && !linked) {
      append("  ");
      if (frame.file[0] != '/') {
        append("ENTRYPOINT/");
      }
      append(frame.file, 1024, true);
      append(":");
      number(frame.line);
      if (frame.column) {
        append(":");
        number(frame.column);
      }
    }
    if (linked) {
      append("\033]8;;\033\\");
    }
    append("\n");
    if (truncated) {
      used = start;
    }
  }
};

/** Renders inline calls as successive levels of the source call chain. */
void appendSourceFrame(const sun::runtime::ExceptionFrameView& frame,
                       void* context) noexcept {
  auto& output = *static_cast<ReportBuffer*>(context);
  output.frame(frame, frame.name, true);
  if (output.throwFrame && frame.throwLabel[0]) {
    output.frame(frame, frame.throwLabel, false);
  }
}

/** Resolves only during reporting and hides host plumbing outside known Sun
 * frames. */
void writeTrace(const std::uintptr_t* frames, std::uint64_t count,
                sun::runtime::TraceStatus status, bool headingWritten,
                bool showThrow = true) noexcept {
  if (!headingWritten) {
    return;
  }
  ReportBuffer output;
  if (count > sun_exception_trace_depth) {
    count = sun_exception_trace_depth;
    status = sun::runtime::TraceStatus::Truncated;
  }
  bool hasSources = false;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (sun::runtime::visitExceptionSource(frames[i], nullptr, nullptr)) {
      hasSources = true;
      break;
    }
  }
  bool started = false;
  for (std::uint64_t i = count; i && !output.truncated; --i) {
    output.throwFrame = showThrow && i == 1;
    if (sun::runtime::visitExceptionSource(frames[i - 1], appendSourceFrame,
                                           &output)) {
      started = true;
    } else if (!hasSources || started) {
      output.framePrefix();
      output.append("0x");
      output.number(frames[i - 1], 16);
      output.append("\n");
    }
  }
  if (output.truncated) {
    constexpr char marker[] = "  ... trace report truncated\n";
    std::memcpy(reportWorkspace + output.used, marker, sizeof(marker) - 1);
    output.used += sizeof(marker) - 1;
  }
  bool ok = writeBytes(reportWorkspace, output.used);
  const char* capture = nullptr;
  switch (status) {
    case sun::runtime::TraceStatus::Complete:
      break;
    case sun::runtime::TraceStatus::Truncated:
      capture = "Trace truncated\n";
      break;
    case sun::runtime::TraceStatus::Partial:
      capture = "Trace partial\n";
      break;
    case sun::runtime::TraceStatus::Unavailable:
    default:
      capture = "Trace unavailable\n";
      break;
    case sun::runtime::TraceStatus::Disabled:
      capture = "Trace disabled\n";
      break;
  }
  if (ok && capture) {
    writeBytes(capture, std::strlen(capture));
  }
}

}  // namespace

void sun_eh_report(const ExceptionValueHeader* value,
                   std::uint32_t unhandled) noexcept {
  if (!value || !value->type ||
      value->trace.count > sun_exception_trace_depth) {
    return;
  }
  const int savedErrno = errno;
  DiagnosticSignals signals;
  if (!signals.ready()) {
    errno = savedErrno;
    return;
  }
  pthread_mutex_lock(&reportMutex);
  ReportBuffer heading;
  heading.append(unhandled ? "Unhandled exception: " : "Exception: ");
  const auto* name = value->type->displayName;
  if (!name) {
    name = value->type->identity;
  }
  heading.append(name ? name : "unknown", sun::runtime::reportNameLimit, true);
  if (name && std::strlen(name) > sun::runtime::reportNameLimit) {
    heading.append("...");
  }
  heading.append("\n");
  const bool ok = writeBytes(reportWorkspace, heading.used);
  writeTrace(value->trace.addresses(), value->trace.count, value->trace.status,
             ok);
  pthread_mutex_unlock(&reportMutex);
  errno = savedErrno;
}

void sun_eh_print_trace(sun::runtime::TraceFrameView view,
                        std::uint32_t status) noexcept {
  const int savedErrno = errno;
  DiagnosticSignals signals;
  if (signals.ready()) {
    pthread_mutex_lock(&reportMutex);
    const bool valid = view.rank == 1 && view.dimensions;
    const auto count = valid ? view.dimensions[0] : 0;
    const auto captureStatus =
        valid ? static_cast<sun::runtime::TraceStatus>(status)
              : sun::runtime::TraceStatus::Unavailable;
    constexpr char heading[] = "Exception trace\n";
    const bool headingWritten = writeBytes(heading, sizeof(heading) - 1);
    writeTrace(view.frames, count, captureStatus, headingWritten);
    pthread_mutex_unlock(&reportMutex);
  }
  errno = savedErrno;
}

__attribute__((noinline)) void sun_report_stack_trace() noexcept {
  const int savedErrno = errno;
  DiagnosticSignals signals;
  if (signals.ready()) {
    /** Reserves bounded capture storage on the calling thread's stack. */
    struct CurrentTrace {
      sun::runtime::StackTraceHeader header;
      std::uintptr_t frames[sun::abi::exceptions::maximumTraceDepth];
    } trace{};

    static_assert(offsetof(CurrentTrace, frames) == sizeof(trace.header));
    const auto caller = reinterpret_cast<std::uintptr_t>(
        __builtin_extract_return_addr(__builtin_return_address(0)));
    sun::runtime::captureStackTrace(trace.header, caller);
    pthread_mutex_lock(&reportMutex);
    constexpr char heading[] = "Stack trace\n";
    const bool written = writeBytes(heading, sizeof(heading) - 1);
    writeTrace(trace.frames, trace.header.count, trace.header.status, written,
               false);
    pthread_mutex_unlock(&reportMutex);
  }
  errno = savedErrno;
}
