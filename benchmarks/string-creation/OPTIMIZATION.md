# String-creation optimizations

The full comparison was rerun locally on September 25, 2026, using the included source files and the optimized Sun checkout. All processes ran sequentially on logical CPU 1 of the Intel Core Ultra 9 185H. See [versions, commands, source hashes, and results](results.json) and [raw output](output.txt).

Sun measured **11.64 ns/string (85.9 million/s)**. CPU frequency and background activity were not locked. All rows below were rerun together with the final implementation.

| Language | ns/string | Million strings/s |
|---|---:|---:|
| C++ std::to_string | 10.18 | 98.2 |
| Rust itoa | 11.20 | 89.3 |
| Sun interpolation | 11.64 | 85.9 |
| Rust to_string() | 12.64 | 79.1 |
| Nim $i | 15.99 | 62.5 |
| Go strconv.Itoa | 17.10 | 58.5 |
| Node.js String(i) | 20.81 | 48.1 |
| Bun String(i) | 28.64 | 34.9 |
| Python str(i) | 50.91 | 19.6 |

![Optimized comparison](strings.png)

## Changes

1. Decimal digits are computed with arithmetic; the per-digit match and its helper call are gone.
2. Integer formatting counts digits, reserves once, and fills the destination backwards. The output needs no reversal. The destination pointer and start offset stay in local variables during writes.
3. Sun uses LLVM's standard O3 module pipeline after linking imports, instead of its previous small custom pass sequence. `-O0` still disables optimization. The machine-code backend retains its existing default optimization setting.
4. String storage now includes an inline byte buffer and a nullable owning heap pointer. Short strings avoid allocation; growth allocates storage and preserves initialized bytes. Borrowed inline addresses are computed on access, so moving a string does not leave a self-pointer into the old object. The inline bytes are represented as machine-sized words to avoid passing each byte as a separate aggregate element.

String API signatures remain the same. The representation grows from 32 to 40 bytes on x86-64, so containers of long strings use more object storage even though short strings avoid separate allocations. The inline buffer holds 16 bytes; `c_str()` needs an additional byte for its terminator and grows when necessary. Moving a String invalidates pointers, views, and iterators into its bytes; that constraint is documented. Libraries carrying the previous String layout must be rebuilt with the new standard library.

## Verification

- All 2,961 enabled CTest cases pass across the full suite and the targeted recheck; two existing lexer benchmarks remain disabled. Three tests were updated to inspect pre-optimization IR or keep their tested symbols live, because O3 legitimately removes unused declarations and folds startup code.
- All 421 standard-library tests pass in an explicitly optimized AOT build.
- All 69 string tests pass under Valgrind, with zero memory errors and no leaks. The added cases cover returned strings, Vec relocation and extraction, alternating heap/inline replacement, independent clones, growth at the inline boundary, NUL termination, negative source lengths, decimal powers of ten, and integer limits.
- A 100,000-conversion allocation probe dropped from 101,026 allocations to 2, including container/runtime overhead, with the same retained-byte checksum. Both versions reported no leaks or memory errors.
- The complete cross-language benchmark passes its checksum checks, and Sun validates every retained integer after warm-up and each timed run.
- The GitHub Actions workflow passes actionlint. Generated binary/cache paths are ignored, while sources, Cargo.lock, and named result snapshots remain trackable.

## Run it

Run `python3 benchmarks/string-creation/run.py --cpu 1` from the repository root with the documented toolchains on PATH. The [README](README.md) describes the included source files, optional chart generation, and the independent informational CI job. CI has been configured and validated locally; it has not been executed on GitHub during this session.
