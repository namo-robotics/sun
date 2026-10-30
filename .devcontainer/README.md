# Development container

Based on `namo-robotics/sun_infra` template `sun-dev` version
`0.1.2`, applied from the local source before registry publication. This is a
reviewed copy, not live inheritance. Compare future template releases before
merging updates into these files.

The project Dockerfile remains responsible for Ubuntu 26.04 and its toolchain.
It declares `USER ubuntu`; the devcontainer inherits that user without overrides.
Startup checks the OS and prepares the cache; it does not update the compiler.
Existing mounts, cache sizes, extensions, and project-specific startup steps are
retained. Fixed CTest worker counts were removed; choose at most half the
available cores (minimum one) when enabling parallel builds or tests.

Open this repository in VS Code and run **Dev Containers: Rebuild and Reopen in
Container** after applying these changes. Each checkout works independently of
`sun_infra`; there is no sibling-repository runtime dependency.

The existing named cache volume is retained instead of adopting the template’s
new per-devcontainer volume name, preserving the current cached data.

The compiler retains its profiling capability, Node feature, and TLS archive
bootstrap after the shared cache setup.
