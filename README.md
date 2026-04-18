<p align="center">
  <img src="assets/sun-logo.svg" alt="Sun Logo" width="120" height="120">
</p>

<h1 align="center">The Sun Programming Language</h1>

<p align="center">
  <a href="https://github.com/namo-robotics/sun/actions/workflows/ci.yml">
    <img src="https://github.com/namo-robotics/sun/actions/workflows/ci.yml/badge.svg" alt="CI">
  </a>
</p>

Sun is an **experimental** programming language exploring Rust-style memory safety while retaining familiar concepts such as classes and interfaces.

## Design Goals

- **Fast**: Systems-level performance without runtime overhead
- **Designed For AI**: NumPy-like N-D matrices and linear algebra provided in the standard library
- **Safety Critical**: Suitable for safety-critical embedded applications, particularly autonomous robotics
- **Memory safe**: No undefined behavior via Rust-style borrow checking
- **Readable**: Straightforward for humans (and AIs!) to read and understand
- **Unambiguous**: No alternative syntaxes. No hidden control flow. No hidden memory allocation. No macros

> **Note: These are _goals_ and not claims about the language's current capabilities**

## Documentation

Full documentation is available at **[namo-robotics.github.io/sun](https://namo-robotics.github.io/sun/)**

## Installation

### Ubuntu (Debian Package)

Download and install the latest dev build:

```bash
curl -LO https://github.com/namo-robotics/sun/releases/download/dev/sun_0.dev_amd64.deb
sudo dpkg -i sun_0.dev_amd64.deb
```

To update, download and install the new package.

### Uninstalling

To uninstall the Debian package:

```bash
sudo dpkg -r sun
```

### Build from Source

```bash
# Clone the repository
git clone https://github.com/namo-robotics/sun.git
cd sun
./build.sh
```

## Quick Start

```sun
// hello.sun
import "stdlib.moon";
using sun;

function main() i32 {
    println("Hello, Sun!");
    return 0;
}
```

### JIT Execution

Set `SUN_PATH` for imports.

```bash
export SUN_PATH=`pwd`/build
```

```bash
ubuntu@5b8bc57b626b:/workspaces/sun$ sun hello.sun
Hello, Sun!
```

### Compile to native executable

```bash
ubuntu@5b8bc57b626b:/workspaces/sun$ sun --compile -o hello hello.sun
Compiling: hello.sun -> hello
Successfully compiled to: hello
```

### Inspect generated LLVM IR

```bash
ubuntu@5b8bc57b626b:/workspaces/sun$ sun --emit-ir hello.sun 
; LLVM IR (user-defined only):
@str = private unnamed_addr constant [12 x i8] c"Hello, Sun!\00", align 1
define void @main() {
entry:
  call void @sun_println(%static_ptr_struct.20 { ptr @str, i64 11 })
  ret void
}

Hello, Sun!
```

## Moons

Sun programs can import precompiled library bundles called **moons** (`.moon` files). Moons contain compiled code and type information, enabling fast compilation and reliable distribution of libraries.

```sun
import "stdlib.moon";

function main() i32 {
    var allocator = make_heap_allocator();
    var m = Matrix<i32>(allocator, [3, 3]);
    m[1, 1] = 42;
    return m[1, 1];
}
```

Sun is designed to work with **Moon**, a safety-critical package manager (coming soon) that will provide:

- **Exact matching**: Dependencies are pinned to the exact hash of the binary and compiler version — every change is a breaking change
- **Verified builds**: Reproducible compilation with cryptographic verification
- **Trusted sources**: Security auditing based on verified, trustable package sources
- **Static bundling**: No dynamic linkage — moons and binaries include all dependencies in a single file

## Running Tests

```bash
cd build && ctest --output-on-failure
```

## Contributing

Contributions are welcome and encouraged! Feel free to open issues and pull requests.
