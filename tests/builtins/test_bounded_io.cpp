/** Checks the compile-time safety contracts of bounded byte I/O. */
#include <gtest/gtest.h>

#include "driver/execution_utils.h"

/** Raw pointer I/O must always require the caller's explicit unsafe contract.
 */
TEST(BoundedIo, raw_file_io_requires_unsafe) {
  for (const auto* method : {"read_into", "write_bytes"}) {
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        sun::driver::executeStringWithStdlib(std::string(R"(
        /** Attempts to use a raw buffer without an unsafe block. */
        function main() i32 {
          var file = std.io.File();
          file.)") + method + R"((null, 1);
          return 0;
        }
      )"),
        "requires an unsafe block");
  }
}

/** A bounded view keeps its backing allocation borrowed until the view's last
 * use. */
TEST(BoundedIo, slice_prevents_moving_storage) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeStringWithStdlib(R"(
using std;

/** Attempts to move storage while a slice still borrows it. */
/** Runs the fixture with explicit error propagation. */
function main() i32 throws {
  var alloc = make_heap_allocator();
  var buffer =  ContiguousBuffer<u8>(alloc, 4);
  var bytes = ByteSlice(buffer);
  var moved = buffer;
  return _convert<i32>(bytes.size());
}
)"),
                                "Borrow check failed");
}

/** Read-only storage cannot be passed to the mutable slice constructor. */
TEST(BoundedIo, mutable_slice_rejects_const_storage) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeStringWithStdlib(R"(
using std;

/** Attempts to acquire write access through a const buffer. */
/** Runs the fixture with explicit error propagation. */
function main() i32 throws {
  var alloc = make_heap_allocator();
  const buffer =  ContiguousBuffer<u8>(alloc, 4);
  var bytes = MutableByteSlice(buffer);
  return _convert<i32>(bytes.size());
}
)"),
                                "const");
}

/** Resizing cannot invalidate storage while a bounded view is still in use. */
TEST(BoundedIo, slice_prevents_resizing_storage) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeStringWithStdlib(R"(
using std;

/** Attempts to replace the allocation while its slice is alive. */
/** Runs the fixture with explicit error propagation. */
function main() i32 throws {
  var alloc = make_heap_allocator();
  var buffer =  ContiguousBuffer<u8>(alloc, 4);
  var bytes = ByteSlice(buffer);
   buffer.resize_to(1);
  return _convert<i32>(bytes.size());
}
)"),
                                "Borrow check failed");
}

/** Read-only methods remain usable while a slice borrows the buffer. */
TEST(BoundedIo, slice_allows_reading_storage_size) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
using std;

/** Reads both sizes while the shared borrow is alive. */
/** Runs the fixture with explicit error propagation. */
function main() i32 throws {
  var alloc = make_heap_allocator();
  var buffer =  ContiguousBuffer<u8>(alloc, 4);
  var bytes = ByteSlice(buffer);
  return _convert<i32>(buffer.size() + bytes.size());
}
)"),
            8);
}

/** Releasing a slice's scope permits replacing the buffer allocation. */
TEST(BoundedIo, storage_can_resize_after_slice_scope) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
using std;

/** Resizes only after the slice and its borrow have ended. */
/** Runs the fixture with explicit error propagation. */
function main() i32 throws {
  var alloc = make_heap_allocator();
  var buffer =  ContiguousBuffer<u8>(alloc, 4);
  if (true) {
    var bytes = ByteSlice(buffer);
    if (bytes.size() != 4) {
      return -1;
    }
  }
   buffer.resize_to(1);
  return _convert<i32>(buffer.size());
}
)"),
            1);
}

/** Mutable views cannot outlive the local allocation they reference. */
TEST(BoundedIo, slice_cannot_escape_local_storage) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeStringWithStdlib(R"(
using std;

/** Attempts to return a dangling slice. */
function escape() MutableByteSlice throws {
  var alloc = make_heap_allocator();
  var buffer =  ContiguousBuffer<u8>(alloc, 4);
  return MutableByteSlice(buffer);
}

/** Forces the invalid function to be checked. */
/** Runs the fixture with explicit error propagation. */
function main() i32 throws {
  var bytes =  escape();
  return _convert<i32>(bytes.size());
}
)"),
                                "cannot return a value that stores references");
}

/** Imported bundles retain the private operations used by the public stream
 * API. */
TEST(BoundedIo, imported_bounded_read) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
using std;

/** Reads initialized bytes through a File imported from stdlib.moon. */
/** Runs the fixture with explicit error propagation. */
function main() i32 throws {
  var alloc = make_heap_allocator();
  var file = std.io.File();
  try { file.open("/dev/zero", std.io.FileMode.Read);
 } catch (ignored_error: Error) {
      return -1;
     } catch (ignored_error: AllocationError) {
      return -1;
     } catch (ignored_error: InvalidArgumentError) {
      return -1;
     } catch (ignored_error: OverflowError) {
      return -1;
     }
  var buffer =  ContiguousBuffer<u8>(alloc, 4);
  var bytes = MutableByteSlice(buffer);
  return _convert<i32>(
    match file.read(bytes) {
      std.io.ReadResult.Data(n) => _convert<i32>(n),
      _ => -2
    }
  );
}
)"),
            4);
}
