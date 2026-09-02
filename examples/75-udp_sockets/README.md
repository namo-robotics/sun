# UDP Sockets

A minimal UDP pair built on the standard library's `UdpSocket`. The
`receiver` binds `127.0.0.1:9090` and prints the first datagram it gets,
along with the sender's address; the `sender` binds an ephemeral port,
sends one message and exits.

Unlike TCP there is no connection: each `send_to` is one self-contained
datagram, and `recv_from` reports who sent it alongside the bytes.

## Build and run

Build both executables:

```bash
./build.sh
```

Run the receiver first, then the sender in a second terminal:

```bash
./receiver   # terminal 1
./sender     # terminal 2
```
