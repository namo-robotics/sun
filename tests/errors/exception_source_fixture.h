/** Provides a small immutable source index without requiring a compiler or
 * debug files. */
#pragma once
#include <cstddef>
#include <cstring>

#include "runtime/exception_sources.h"

/** Supplies deterministic source metadata for reporting and allocation-denial
 * tests. */
namespace sun::tests::exceptions {
/** Registers three call frames for this scope and removes them before their
 * storage expires. */
template <std::size_t FrameCapacity = 3>
struct SourceIndexFixture {
  static_assert(FrameCapacity >= 3);
  runtime::ExceptionSourceMap map;
  runtime::ExceptionSourceFunction functions[3]{};
  runtime::ExceptionSourceRow rows[3]{};
  runtime::ExceptionSourceFrame frames[FrameCapacity]{};
  char names[3][512]{};
  char file[128] = "/tmp/report example.sun";
  char thrown[32] = "throw Failure()";
  char empty[1]{};

  /** Publishes main, foo, and bar with known source lines and a final throw. */
  SourceIndexFixture() : map{nullptr, 3, sizeof(*this)} {
    const char* labels[] = {"main", "foo", "bar"};
    for (unsigned i = 0; i < 3; ++i) {
      std::strcpy(names[i], labels[i]);
      functions[i] = {0x100u * (i + 1), 16,
                      std::uint32_t(offsetof(SourceIndexFixture, rows) +
                                    i * sizeof(rows[0])),
                      1};
      rows[i] = {0, 16,
                 std::uint32_t(offsetof(SourceIndexFixture, frames) +
                               i * sizeof(frames[0])),
                 1};
      frames[i] = {std::uint32_t(offsetof(SourceIndexFixture, names) +
                                 i * sizeof(names[0])),
                   offsetof(SourceIndexFixture, file), 10u * (i + 1), 2,
                   std::uint32_t(i == 2 ? offsetof(SourceIndexFixture, thrown)
                                        : offsetof(SourceIndexFixture, empty))};
    }
    sun_eh_register_source_map(&map);
  }

  /** Unregisters before the borrowed strings and rows disappear. */
  ~SourceIndexFixture() { sun_eh_unregister_source_map(&map); }

  /** Registered metadata cannot be copied to a different address. */
  SourceIndexFixture(const SourceIndexFixture&) = delete;
  /** Registered metadata cannot be reassigned. */
  SourceIndexFixture& operator=(const SourceIndexFixture&) = delete;
};

static_assert(offsetof(SourceIndexFixture<>, functions) ==
              sizeof(runtime::ExceptionSourceMap));
}  // namespace sun::tests::exceptions
