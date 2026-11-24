<p align="center">
  <img src="assets/sun_banner.jpg" alt="Sun Logo">
</p>

<h1 align="center">The Sun Programming Language</h1>

<p align="center">
  <a href="https://github.com/namo-robotics/sun/actions/workflows/ci.yml">
    <img src="https://github.com/namo-robotics/sun/actions/workflows/ci.yml/badge.svg" alt="CI">
  </a>
</p>

<p align="center">
  <a href="https://namo-robotics.github.io/sun/">Documentation</a>
</p>

## Incremental builds

Sun hashes file contents with BLAKE3 before comparing build inputs. Successful
Moon payload validation is cached under `.sun/cache/validation/v1` in the selected
workspace; custom build compiler processes share that directory. Changes to bundle
bytes, the compiler, compiler runtime libraries, target, or compilation settings
require fresh validation. File timestamps are not used to authorize reuse.

Set `SUN_VALIDATION_CACHE` to choose a different directory, or
`SUN_DISABLE_VALIDATION_CACHE=1` to disable validation caching. `--force-rebuild`
bypasses validation receipts and rebuilds selected products. Cache files are
local disposable build state: removing them triggers validation again. Use
`SUN_LOG_LEVEL=debug` to inspect validation cache hits and full-validation counts.

The BLAKE3 migration changes the Moon format, build records, package inventories,
and generated symbol identities. Rebuild libraries, programs, and packages with
the new compiler; old artifacts are not interchangeable. External dependency
fields named `sha256` (and legacy download `hash` fields) still verify SHA-256.
