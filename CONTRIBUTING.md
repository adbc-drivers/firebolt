<!--
Copyright (c) 2026 ADBC Drivers Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Contributing

## Prerequisites

Docker and git. That is all — the toolchain, the compiler, and every dependency
live inside a builder image or a git submodule, so nothing needs installing on
the host.

For the linters, also [pre-commit](https://pre-commit.com/), Python 3.14 and a
Java runtime (the license check runs Apache RAT); see [Linting](#linting).

## Build and test

The three scripts under `scripts/` are the canonical entry points. CI runs the
same commands, so a green local run means a green CI run.

```bash
./scripts/build.sh              # → build/libadbc_driver_firebolt.so (+ unit test binary)
./scripts/test-unit.sh          # C++ unit tests via ctest, no server needed
./scripts/test-integration.sh   # pytest against a throwaway 1-node engine
```

All three are idempotent and can be run from anywhere in the repository.

`scripts/build.sh` initialises the submodules, then configures and builds inside
`firebolt-adbc-builder:latest`, which it builds from `docker/builder/Dockerfile`
on first use. The image is Ubuntu 22.04 + clang-18; the older glibc is
deliberate, so the resulting `.so` loads on distributions older than your host.
Delete the image (`docker rmi firebolt-adbc-builder:latest`) to force a rebuild.

### Linting

[pre-commit](https://pre-commit.com/) runs the linters configured in
`.pre-commit-config.yaml`: clang-format, clang-tidy, ruff, shellcheck, codespell,
whitespace and YAML checks, and the Foundry's license-header check (`rat`).

```bash
pre-commit install              # run the hooks on every git commit
pre-commit run --all-files      # or run them all by hand
./scripts/clang-tidy.sh         # clang-tidy alone, over all of src/ and tests/unit/
```

clang-tidy reads `build/compile_commands.json` and runs inside the builder
image, so it needs a `./scripts/build.sh` first. If the image was built before
clang-tidy was added to it, delete it and re-run the build. Its checks, in
`.clang-tidy`, treat every warning as an error.
`SKIP=clang-tidy git commit` skips it when there is no build at hand; CI runs it
regardless.

A file that cannot carry a header (JSON) is listed in `.rat-excludes`; a file
taken from an Apache project is listed in `.rat-apache`.

### Running a subset of the tests

```bash
./scripts/test-integration.sh tests/dml                 # one suite
./scripts/test-integration.sh -k test_connect           # by test name
./scripts/test-integration.sh -x                        # stop at first failure
./scripts/test-integration.sh --engine-image=...:tag    # a different engine build

cd build && ./adbc_driver_tests --gtest_filter='DatabaseOptionTest.*'
```

The engine image is pulled fresh on every run, because it is a floating `:latest`
tag; a cached copy is only used if the pull fails.

### Working against a long-lived engine

For iterating on a single behaviour it is quicker to keep one engine up and talk
to it directly:

```bash
docker run -d --name firebolt -p 3473:3473 ghcr.io/firebolt-db/engine:latest
python3 examples/python/quickstart.py
```

### Building outside Docker

Supported, and faster to iterate on, but the resulting `.so` carries your host's
glibc requirement — never ship it.

```bash
git submodule update --init --recursive
cmake --preset standalone-clang -DFIREBOLT_ADBC_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
```

If you have configured `build/` this way and then run `scripts/build.sh`, the
Docker build can fail on cached host paths — a `ccache` launcher baked into
`CMakeCache.txt`, for instance, which does not exist in the builder image.
Delete `build/` and re-run.

## Conventions

- **Keep `pre-commit run --all-files` clean.** It formats C++ (`.clang-format`)
  and Python (ruff), and CI fails a PR that it would change.
- **Every new file starts with the Apache 2.0 header** — `Copyright (c) 2026 ADBC
  Drivers Contributors` and the standard notice, in the file's comment syntax
  (copy it from a neighbour). A file taken from another Apache-licensed project,
  such as the vendored `adbc.h`, keeps its ASF header below the copyright line
  and a "This file has been modified from its original version, which is under
  the Apache License:" line, and is listed in `.rat-apache`.
- **No dependency discovery in CMake.** No `find_package`, `find_library`,
  `find_path`, `find_program`, or `FetchContent` for required dependencies —
  everything must exist under `submodule/`. See [CLAUDE.md](CLAUDE.md) for why.
- **No packdb internal headers.** The `.so` has to load outside the server
  process.
- **Only `AdbcDriverInit` and `AdbcDriverFireboltInit` are exported**, enforced by
  `adbc_driver_firebolt.version`. If you add a public entry point, add it there too.
- **Test the failure, not just the success.** Every option the driver accepts has
  a wrong value someone will pass; the interesting test is what happens then. An
  exception must never escape through the C ABI — it aborts the host process.

## Adding a test

| Kind | Where | Notes |
|------|-------|-------|
| C++ unit | `tests/unit/adbc_driver_test.cpp` | No server. Good for option handling, SQL generation, type mapping. |
| Integration | `tests/integration/tests/<area>/test.py` | One directory per area, each with `__init__.py` and `test.py`. Fixtures come from `tests/integration/conftest.py`. |

The shared fixtures are worth reading before writing anything new:
`conn`, `dbapi_conn`, `cursor`, `run_query`, `ingest`, `table_name`,
`temp_table`, `scratch_table`, and `mock_server` for responses a real engine will
not produce.

Generated artifacts under `tests/integration/_test_runtime_root/` are recreated
every run and are gitignored.

## Versioning and releases

`project(adbc_driver_firebolt VERSION ...)` in `CMakeLists.txt` is the single source of
truth. It feeds the shared library soname and the `ADBC_INFO_DRIVER_VERSION`
string through the generated `src/Version.h.in`, so there is one number to bump.

To release: bump that version, update [CHANGELOG.md](CHANGELOG.md), merge, then
tag.

```bash
git tag v0.2.0 && git push origin v0.2.0
```

`.github/workflows/release.yaml` refuses to publish if the tag and the CMake
version disagree, then builds for x86_64 and aarch64 and attaches the `.so`,
its `sha256`, and `firebolt.toml` to the release.

## Documentation that has to stay true

A change to the driver's surface usually means a change to one of these. They
make specific claims, and a stale claim is worse than no claim:

| File | Contains |
|------|----------|
| [README.md](README.md) | "Supported today", the Feature & Type Support tables, troubleshooting table |
| [OPTIONS.md](OPTIONS.md) | Every option, every error status, the type mapping |
| [docs/authentication.md](docs/authentication.md) | What works now and the gaps against the SDK auth spec |
| [CLAUDE.md](CLAUDE.md) | Architecture and design decisions |

## Pull requests

`enable-merge-to-main.yaml` runs the pre-commit hooks, build, clang-tidy, unit
tests, integration tests, and the examples smoke check on every PR, with a merge gate that requires all of it to
pass.
