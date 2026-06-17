// intrinsics/io.h — File I/O intrinsic definitions
//
// File I/O intrinsics wrap Linux syscalls directly (no libc).
// All functions use raw file descriptors (i32).

#pragma once

namespace sun {

// File I/O intrinsic identifiers are defined in the main Intrinsic enum.
// This header exists for organizational purposes and future expansion.
//
// File operations:
//   __file_open(path, flags) -> i32       Open file, returns fd
//   __file_close(fd) -> i32               Close file descriptor
//   __file_write(fd, data) -> i32         Write string to fd
//   __file_read(fd, size) -> raw_ptr<i8>  Read size bytes from fd
//   __write(fd, buf, len) -> i64          Write len bytes from buf
//   __read(fd, buf, len) -> i64           Read len bytes into buf
//
// Seek/position:
//   __lseek(fd, offset, whence) -> i64    Seek to position
//
// File metadata:
//   __fstat(fd, stat_buf) -> i32          Get file status
//   __fsync(fd) -> i32                    Flush to disk
//   __ftruncate(fd, length) -> i32        Truncate/extend file
//
// Directory operations:
//   __unlink(path) -> i32                 Remove file
//   __rename(old, new) -> i32             Rename/move file
//   __mkdir(path, mode) -> i32            Create directory
//   __rmdir(path) -> i32                  Remove directory

}  // namespace sun
