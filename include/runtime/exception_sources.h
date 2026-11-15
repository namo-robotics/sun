/** Describes compiler-prepared address indexes used by allocation-free
 * reporting. */
#pragma once
#include <cstddef>
#include <cstdint>

/** Shares the source-index layout between object emission and runtime lookup.
 */
namespace sun::runtime {
inline constexpr std::size_t reportWorkspaceSize = 32 * 1024;
inline constexpr std::size_t reportNameLimit = 160;

/** Starts one module's index; offsets below are relative to this header. */
struct ExceptionSourceMap {
  ExceptionSourceMap* next;
  std::uint32_t functionCount;
  std::uint32_t byteSize;
};

/** Relocates with its function, so the index works with ASLR and JIT code. */
struct ExceptionSourceFunction {
  std::uintptr_t address;
  std::uint64_t size;
  std::uint32_t rows;
  std::uint32_t rowCount;
};

/** Associates an instruction interval with an outermost-first inline call
 * chain. */
struct ExceptionSourceRow {
  std::uint32_t begin;
  std::uint32_t end;
  std::uint32_t frames;
  std::uint32_t frameCount;
};

/** Refers to strings in the module index without depending on runtime
 * allocation. */
struct ExceptionSourceFrame {
  std::uint32_t name;
  std::uint32_t file;
  std::uint32_t line;
  std::uint32_t column;
  std::uint32_t throwLabel;
};

/** Borrows a resolved frame only for the duration of the visitor call. */
struct ExceptionFrameView {
  const char* name;
  const char* file;
  const char* throwLabel;
  std::uint32_t line;
  std::uint32_t column;
};

/** Formats resolved metadata while its owning module remains registered. */
using ExceptionFrameVisitor = void (*)(const ExceptionFrameView&,
                                       void*) noexcept;
/** Resolves an address without allocation; invokes the visitor for each inline
 * frame. */
bool visitExceptionSource(std::uintptr_t address, ExceptionFrameVisitor visitor,
                          void* context) noexcept;
}  // namespace sun::runtime

extern "C" {
/** Publishes an immutable source index before its generated code executes. */
void sun_eh_register_source_map(void* map) noexcept;
/** Removes an index before JIT code and its metadata are released. */
void sun_eh_unregister_source_map(void* map) noexcept;
}
