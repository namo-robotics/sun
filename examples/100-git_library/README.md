# Build dependency from Git source

Fetches [sun_serve](https://github.com/namo-robotics/sun_serve), builds its Moon
library, and links a program that checks HTTP status helpers and prints the
library version and `OK`.

Requires x86_64 Linux, Git, Sun with `stdlib.moon`, and musl archives
`libz.a`, `libssl.a`, and `libcrypto.a`. Run `scripts/fetch-openssl.sh` to
install these under `third_party/openssl/x86_64-linux-musl`, the config default.
The Docker image supplies them through `SUN_EXAMPLE_NATIVE_LIBS`; set that
variable to use another directory when running `build.sh`. The fetched
repository stays unchanged. Explicit `--path-var` arguments override the
build script and config defaults. Build and test scripts explicitly skip
unsupported platforms.

From the workspace root:

```bash
examples/100-git_library/build.sh
bash examples/100-git_library/test.sh
```

Unchanged builds skip compilation. `version` accepts a branch, tag, or commit;
the cached revision stays fixed until refreshed:

```bash
examples/100-git_library/build.sh --refresh-sources
```

Sources are cached in `~/.sun/cache/git`, overridable with `SUN_GIT_CACHE`.
For SSH, set `git` to `git@github.com:namo-robotics/sun_serve.git`.
To use a precompiled Moon, remove the Git entrypoint and supply the bundle
through `sun_path`; `main.sun` stays unchanged.
