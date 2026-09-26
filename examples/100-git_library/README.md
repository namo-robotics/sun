# Build dependency from Git source

Fetches [sun_serve](https://github.com/namo-robotics/sun_serve), builds its Moon
library, and links a program that checks HTTP status helpers and prints the
library version and `OK`.

Requires x86_64 Linux, Git, Sun with `stdlib.moon`, and musl archives
`libz.a`, `libssl.a`, and `libcrypto.a`. Set `MUSL_LIB` in this example's
`sun-config.json` to their directory (default: `build/native`). The fetched
repository stays unchanged; this config overrides its defaults, and explicit
`--path-var` arguments override both.

From the workspace root:

```bash
examples/100-git_library/build.sh
examples/100-git_library/build/git_library
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
