// intrinsics/network.h — Network socket intrinsic definitions
//
// Socket intrinsics call libc (see intrinsics/libc.h).
// All functions use raw file descriptors (i32) for sockets.

#pragma once

namespace sun {

// Network intrinsic identifiers are defined in the main Intrinsic enum.
// This header exists for organizational purposes and future expansion.
// These calls follow libc's contract: a failing socket operation returns -1
// and leaves its positive error code in errno. A wrapper must read errno
// immediately, before making another C call.
//
// Socket creation/connection:
//   __socket(domain, type, protocol) -> i32
//     Create a socket. Returns its descriptor, or -1 on failure.
//     domain: AF_INET=2, AF_INET6=10, AF_UNIX=1
//     type: SOCK_STREAM=1, SOCK_DGRAM=2
//     protocol: usually 0
//
//   __bind(fd, addr, addrlen) -> i32
//     Bind socket to address. addr is raw_ptr<u8> to sockaddr struct.
//
//   __listen(fd, backlog) -> i32
//     Mark socket as listening. backlog = max pending connections.
//
//   __accept(fd, addr, addrlen) -> i32
//     Accept incoming connection. Returns new socket fd.
//     addr/addrlen can be null/0 if client address not needed.
//
//   __connect(fd, addr, addrlen) -> i32
//     Connect to remote address.
//
// Data transfer:
//   __send(fd, buf, len, flags) -> i64
//     Send data on connected socket. Returns bytes sent, or -1 on failure.
//
//   __recv(fd, buf, len, flags) -> i64
//     Receive data from socket. Returns bytes received, or -1 on failure.
//
// Socket control:
//   __shutdown(fd, how) -> i32
//     Shutdown socket. how: SHUT_RD=0, SHUT_WR=1, SHUT_RDWR=2
//
//   __setsockopt(fd, level, optname, optval, optlen) -> i32
//     Set socket option. Common: SO_REUSEADDR at SOL_SOCKET level.
//
//   __getsockopt(fd, level, optname, optval, optlen) -> i32
//     Get socket option.

// Socket address constants (for stdlib wrappers)
// AF_INET = 2, AF_INET6 = 10, AF_UNIX = 1
// SOCK_STREAM = 1, SOCK_DGRAM = 2
// SOL_SOCKET = 1, SO_REUSEADDR = 2

}  // namespace sun
