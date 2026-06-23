# TCP Connection

This example demonstrates how to create a raw TCP connection and send data through it.

Compile the executable:

```bash
./build.sh
```

Run netcat listener:
```bash
nc -l -p 8080 -k
```

The run the executable in a new terminal:

```bash
./main
```
