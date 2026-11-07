# Build dependency from Git source

Fetches a pinned revision of [sun_serve](https://github.com/namo-robotics/sun_serve),
builds `src/sun_serve.sun` with the consumer's compiler settings, and links a program
that checks HTTP status helpers and polling results, then prints the library
version and `OK`. The pinned revision uses `ServerResult<T>`: the consumer
propagates setup and polling failures with prefix `try`, handles an expected
invalid-descriptor error with `match`, and reports unexpected errors from `main`.

Requires x86_64 Linux, Git, Sun with `stdlib.moon`, and musl archives
`libz.a`, `libssl.a`, and `libcrypto.a`. Run `scripts/fetch-openssl.sh` to
install these under `third_party/openssl/x86_64-linux-musl`, the config default.
The Docker image supplies them through `SUN_EXAMPLE_NATIVE_LIBS`; set that
variable to use another directory when running `build.sh`. The fetched
repository stays unchanged. Explicit `--path-var` arguments override the
build script and config defaults. Build and test scripts explicitly skip
unsupported platforms.

The pinned revision predates package IDs, so this example selects its source
file directly with `path`. It does not read the dependency's `sun-config.json`.
The consumer imports `"$SUN_SERVE/lib/sun_serve.moon"` from the isolated build
cache. No prebuilt bundle or manual checkout is needed.

The consumer's `sun_path` takes precedence over the dependency's search paths.
This example includes `../../build`, so both `sun_serve` and the example use your
workspace's `build/stdlib.moon` when present. Build it from your local sources:

```bash
build/sun --emit-moon -o build/stdlib.moon stdlib/stdlib.sun
```

For another Sun checkout, set `sun_path` to its build directory instead.

From the workspace root, starting without the native archives:

```bash
bash scripts/fetch-openssl.sh
examples/100-git_library/build.sh
bash examples/100-git_library/test.sh
```

Unchanged builds skip compilation. `version` accepts a branch, tag, or commit;
the example pins a commit for repeatable builds. Branches and tags remain cached
until refreshed:

```bash
examples/100-git_library/build.sh --refresh-sources
```

Sources are cached in `.sun/cache/git` beneath the directory where Sun is run,
overridable with `SUN_GIT_CACHE`.
For SSH, set `git` to `git@github.com:namo-robotics/sun_serve.git` or
`ssh://git@github.com/namo-robotics/sun_serve.git`; both use Git's normal SSH credentials.
To use a precompiled Moon, replace the source dependency with a
`{"package": "SUN_SERVE", "library": "sun_serve", "path": "sun_serve.moon"}`
artifact declaration. Configured Git dependencies instead use a repository's
`packages` dictionary and require package IDs in that repository's configs.
