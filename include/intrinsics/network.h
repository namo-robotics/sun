// intrinsics/network.h — Network socket intrinsic definitions
//
// Socket intrinsics wrap Linux syscalls directly (no libc).
// All functions use raw file descriptors (i32) for sockets.
//
// Linux x86_64 syscall numbers:
//   SYS_SOCKET    = 41
//   SYS_CONNECT   = 42
//   SYS_ACCEPT    = 43
//   SYS_SENDTO    = 44
//   SYS_RECVFROM  = 45
//   SYS_SHUTDOWN  = 48
//   SYS_BIND      = 49
//   SYS_LISTEN    = 50
//   SYS_SETSOCKOPT = 54
//   SYS_GETSOCKOPT = 55

#pragma once

namespace sun {

// Network intrinsic identifiers are defined in the main Intrinsic enum.
// This header exists for organizational purposes and future expansion.
//
// Socket creation/connection:
//   __socket(domain, type, protocol) -> i32
//     Create a socket. Returns fd or negative errno.
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
//     Send data on connected socket. Returns bytes sent or -errno.
//
//   __recv(fd, buf, len, flags) -> i64
//     Receive data from socket. Returns bytes received or -errno.
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
