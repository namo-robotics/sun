// tests/stdlib/test_udp.cpp - UDP sockets and IPv4 addresses
//
// Exercises UdpSocket and Ipv4Addr from stdlib/networking.sun. Every socket
// binds port 0 and reads the OS's pick back with local_port(), so the tests
// need no fixed ports and no native helper sockets.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Stdlib_Udp, roundtrip_loopback) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var alloc = make_heap_allocator();
        var here = ipv4_loopback();
        var sender = UdpSocket();
        var receiver = UdpSocket();
        try {
            sender.bind(here, 0);
            receiver.bind(here, 0);
            var sent: i64 = sender.send_str_to("ping", here, receiver.local_port());
            if (sent != 4) { return 1; }
            var buf = ContiguousBuffer<u8>(alloc, 64);
            var datagram = receiver.recv_from(buf);
            if (datagram.length() != 4) { return 2; }
            if (datagram.port() != sender.local_port()) { return 3; }
            if (datagram.addr().to_network_i32() != INADDR_LOOPBACK) { return 4; }
            var first: u8 = unsafe { _load<u8>(buf.raw_data(), 0); };
            var last: u8 = unsafe { _load<u8>(buf.raw_data(), 3); };
            if (first != 112) { return 5; }  // 'p'
            if (last != 103) { return 6; }   // 'g'
            sender.close();
            receiver.close();
            return 0;
        } catch (e: IError) {
            return -1;
        }
    }
  )");

  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Udp, recv_timeout_reports_would_block) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.time;

    function main() i32 {
        var alloc = make_heap_allocator();
        var here = ipv4_loopback();
        var socket = UdpSocket();
        try {
            socket.bind(here, 0);
            var timeout = create_duration_millis(50);
            socket.set_recv_timeout(timeout);
            var buf = ContiguousBuffer<u8>(alloc, 16);
            try {
                socket.recv_from(buf);
                return 1;
            } catch (e: IError) {
                if (not is_would_block(e.code())) { return 2; }
            }
            socket.close();
            return 0;
        } catch (e: IError) {
            return 3;
        }
    }
  )");

  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Udp, multicast_options_smoke) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var socket = UdpSocket();
        var wildcard = ipv4_any();
        var group = Ipv4Addr(224, 0, 0, 251);
        var iface = ipv4_loopback();
        try {
            socket.bind(wildcard, 0);
            socket.join_multicast(group, iface);
            socket.set_multicast_ttl(1);
            socket.set_multicast_loop(true);
            socket.set_multicast_loop(false);
            socket.leave_multicast(group, iface);
            socket.close();
            return 0;
        } catch (e: IError) {
            return 1;
        }
    }
  )");

  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Udp, ipv4addr_roundtrip) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var alloc = make_heap_allocator();
        var loopback = Ipv4Addr(127, 0, 0, 1);
        if (loopback.to_network_i32() != INADDR_LOOPBACK) { return 1; }
        if (loopback.octet(0) != 127) { return 2; }
        if (loopback.octet(3) != 1) { return 3; }
        var lan = Ipv4Addr(192, 168, 1, 10);
        var text = lan.to_string(alloc);
        if (not text.equals_literal("192.168.1.10")) { return 4; }
        try {
            var parsed = parse_ipv4(text);
            if (parsed.to_network_i32() != lan.to_network_i32()) { return 5; }
        } catch (e: IError) {
            return 6;
        }
        var truncated = String(alloc, "192.168.1");
        try {
            parse_ipv4(truncated);
            return 7;
        } catch (e: IError) {
        }
        var overflow = String(alloc, "1.2.3.400");
        try {
            parse_ipv4(overflow);
            return 8;
        } catch (e: IError) {
        }
        return 0;
    }
  )");

  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Udp, send_on_closed_socket_reports_errno) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var socket = UdpSocket();
        var dest = ipv4_loopback();
        try {
            socket.send_to(null, 0, dest, 9);
            return 1;
        } catch (e: IError) {
            // EBADF is 9 on every supported target.
            if (e.code() != 9) { return 2; }
        }
        return 0;
    }
  )");

  EXPECT_EQ(value, 0);
}
