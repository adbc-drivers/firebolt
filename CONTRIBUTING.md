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

All contributors are expected to follow the [Code of
Conduct](https://github.com/adbc-drivers/firebolt?tab=coc-ov-file#readme).

## Reporting Issues and Making Feature Requests

Please file issues and feature requests on the GitHub issue tracker:
https://github.com/adbc-drivers/firebolt/issues

Potential security vulnerabilities should be reported to
[security@adbc-drivers.org](mailto:security@adbc-drivers.org), not in a
public issue. See the [Security
Policy](https://github.com/adbc-drivers/firebolt?tab=security-ov-file#readme).

## Prerequisites

Docker, git, and [Pixi](https://pixi.sh/). The compiler and native dependencies
live inside a shared builder image or a git submodule.

For the linters, also [pre-commit](https://pre-commit.com/), Python 3.14 and a
Java runtime (the license check runs Apache RAT); see [Linting](#linting).

## Build and test

The generated ADBC build entry point builds a native library for the host:

```bash
git submodule update --init --recursive
pixi run make                   # .so on Linux, .dylib on macOS, .dll on Windows
```

For a debug build with C++ unit tests, call the same hooks used by generated CI.
For example, on Apple Silicon macOS:

```bash
./ci/scripts/build.sh test macos arm64
./ci/scripts/test.sh macos arm64
```

Replace the platform and architecture arguments as needed. These commands are
idempotent and can be run from anywhere in the repository.

The validation suite uses the native artifact from `pixi run make`; the Firebolt
engine itself runs in Docker. See [validation/README.md](validation/README.md) for
the direct Compose and Pixi commands and details about selecting another engine
image. Its generated CI job is currently disabled while conformance gaps are
triaged.

Linux release builds use the shared `adbc-drivers/dev` manylinux_2_28 C++ image.
This gives release binaries a glibc 2.28 compatibility baseline without
maintaining a repository-specific builder image.

### Linting

[pre-commit](https://pre-commit.com/) runs the linters configured in
`.pre-commit-config.yaml`: clang-format, clang-tidy, ruff, shellcheck, codespell,
whitespace and YAML checks, and the Foundry's license-header check (`rat`).

```bash
pre-commit install              # run the hooks on every git commit
pre-commit run --all-files      # or run them all by hand
./scripts/clang-tidy.sh         # clang-tidy alone, over all of src/ and tests/unit/
```

clang-tidy reads the generated Linux test build's `compile_commands.json` and
runs with `run-clang-tidy-18` from the Linux CI host. A checkout-only pre-commit
run skips it when that build does not exist; generated build CI invokes it after
compilation. Its checks, in `.clang-tidy`, treat every warning as an error.

A file that cannot carry a header (JSON) is listed in `.rat-excludes`; a file
taken from an Apache project is listed in `.rat-apache`.

### Running a subset of the tests

```bash
./tests/integration/runner.py tests/dml                 # one suite
./tests/integration/runner.py -k test_connect           # by test name
./tests/integration/runner.py -x                        # stop at first failure
./tests/integration/runner.py --engine-image=...:tag -x # a different engine build

./build/ci-test-macos-arm64/adbc_driver_tests --gtest_filter='DatabaseOptionTest.*'
```

The requested engine image is pulled before every run; a cached copy is only
used if the pull fails.

### Working against a long-lived engine

For iterating on a single behaviour it is quicker to keep one engine up and talk
to it directly:

```bash
docker run -d --name firebolt -p 3473:3473 ghcr.io/firebolt-db/engine:5.0.0-pre.0.20260927210425.e91cd5bd17f8
python3 examples/python/quickstart.py
```

### Building outside Docker

`pixi run make` builds natively on macOS and Windows. On Linux, a direct call to
the CI build hook is useful for quick iteration, but its `.so` carries the host's
glibc requirement and must not be released:

```bash
git submodule update --init --recursive
./ci/scripts/build.sh test linux amd64
./ci/scripts/test.sh linux amd64
```

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
  everything must exist under `submodule/`. See [AGENTS.md](AGENTS.md) for why.
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

`.github/workflows/script_release.yaml` builds and packages tagged releases.
Each release archive gets a combined license from
`ci/scripts/generate_license.sh`; update that script when a runtime dependency
is added or removed.

The CI workflows and `pixi.toml` are generated from
`.github/workflows/generate.toml`. Regenerate them from the repository root:

```bash
uvx --from git+https://github.com/adbc-drivers/dev adbc-gen-workflow generate "$(pwd)"
```

## Documentation that has to stay true

A change to the driver's surface usually means a change to one of these. They
make specific claims, and a stale claim is worse than no claim:

| File | Contains |
|------|----------|
| [README.md](README.md) | "Supported today", the Feature & Type Support tables, troubleshooting table |
| [OPTIONS.md](OPTIONS.md) | Every option, every error status, the type mapping |
| [docs/authentication.md](docs/authentication.md) | What works now and the gaps against the SDK auth spec |
| [AGENTS.md](AGENTS.md) | Architecture and design decisions |

## Pull requests

`script_test.yaml` runs the generated build, unit, and packaging jobs on every
pull request. The existing Firebolt-specific integration suite can be run
locally; shared validation remains disabled in CI.
