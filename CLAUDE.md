# CLAUDE.md — Firebolt ADBC Driver

## Project Overview

A standalone C++ shared library (`libfirebolt_adbc.so`) implementing the
[ADBC 1.1.0](https://arrow.apache.org/adbc/) C API against Firebolt's HTTP query
interface. Client-side only — loaded at runtime by ADBC driver managers (Python
`adbc_driver_manager`, R `adbcdrivermanager`, etc.).

Lives at `adbc/` in the packdb repo root but is a **completely independent CMake
project** with no coupling to the packdb build system.

## Directory layout

```
adbc/
├── README.md                              # user-facing overview
├── OPTIONS.md                             # full options reference (all three levels)
changelog
├── CLAUDE.md                              # this file
│
├── CMakeLists.txt                         # standalone CMake project
├── CMakePresets.json                      # presets: standalone-clang / standalone-gcc
├── firebolt_adbc.version                  # linker version script (exports only AdbcDriverInit)
├── adbc.h                                 # vendored ADBC 1.1.0 C header + Arrow C ABI structs
├── submodule/                             # all non-system deps (git submodules + local static builds)
│
├── src/                                   # driver implementation
│   ├── FireboltAdbcDriver.cpp             # ADBC function table, entry point, curl init/cleanup
│   ├── FireboltAdbcDatabase.h             # FireboltDatabase struct (AdbcDatabase::private_data)
│   ├── FireboltAdbcConnection.h           # FireboltConnection struct; owns HttpClient + session state
│   ├── FireboltAdbcStatement.h            # FireboltStatement struct; holds SQL + bound IPC bytes
│   ├── FireboltAdbcMetadata.h/.cpp        # GetInfo, GetTableTypes, GetTableSchema, GetObjects
│   ├── HttpClient.h/.cpp                  # libcurl wrapper: query POST, multipart insert, session headers
│   └── ArrowIpcStream.h/.cpp              # Arrow IPC bytes → ArrowArrayStream via nanoarrow 0.8.0
│
├── scripts/                               # locally runnable, idempotent — also called from CI
│   ├── build.sh                           # submodule init + cmake + ninja (with -DFIREBOLT_ADBC_BUILD_TESTS=ON)
│   ├── test-unit.sh                       # ctest --output-on-failure in build/
│   └── test-integration.sh                # forwards args to tests/integration/runner.py
│
├── .github/
│   └── workflows/
│       └── enable-merge-to-main.yaml      # build + unit + integration on every PR
│
└── tests/
    ├── unit/
    │   └── adbc_driver_test.cpp           # Google Test unit tests (no server needed)
    └── integration/                       # pytest harness — runs inside a runner container
        ├── runner.py                      # spins up runner image + 1-node Firebolt Core,
        │                                  #   exec's pytest. Args: --core-image, --adbc-binary
        ├── conftest.py                    # shared fixtures: started_core, server_url, conn,
        │                                  #   run_query, table_name, temp_table
        ├── pytest.ini                     # python_files = test.py
        ├── docker/
        │   ├── Dockerfile                 # firebolt-adbc-integration-test-runner image
        │   └── requirements.txt           # adbc-driver-manager, pyarrow, pytest, requests
        ├── helpers/
        │   ├── __init__.py
        │   └── firebolt_core.py           # minimal docker-compose 1-node Firebolt Core fixture
        └── tests/                         # one directory per test area
            ├── adbc_sanity/test.py        # connectivity, literal selects, arithmetic, strings
            ├── advanced_queries/test.py   # joins, CTEs, CASE, HAVING, subqueries, window funcs
            ├── array_type/test.py         # ARRAY literals, roundtrip, functions, nested arrays
            ├── arrow/test.py              # schema, result shape, error handling, C-stream
            ├── datatypes/test.py          # int/float/string/bool/null/date/timestamp scalars
            ├── decimal_type/test.py       # DECIMAL literals, arithmetic, precision/scale
            └── dml/test.py                # DDL, INSERT/SELECT, aggregates, type roundtrip
```

## Build

### Recommended (clang-18 + adbc/submodule deps)

```bash
git submodule update --init --recursive adbc/submodule
cmake --preset standalone-clang -S adbc
cmake --build adbc/build -j$(nproc)
# → adbc/build/libfirebolt_adbc.so
```

### With tests

```bash
cmake --preset standalone-clang -S adbc -DFIREBOLT_ADBC_BUILD_TESTS=ON
cmake --build adbc/build -j$(nproc)
```

### GCC preset (same dependency policy)

```bash
cmake --preset standalone-gcc -S adbc
cmake --build adbc/build -j$(nproc)
```

## Testing

The reusable scripts under `scripts/` are the canonical entry points — both
local development and CI invoke the same commands.

```bash
# Build with -DFIREBOLT_ADBC_BUILD_TESTS=ON (idempotent):
./scripts/build.sh

# C++ unit tests (no server needed):
./scripts/test-unit.sh

# Integration tests against a 1-node Firebolt Core (Docker required):
./scripts/test-integration.sh                                    # all tests
./scripts/test-integration.sh -k test_connect                    # filter by name
./scripts/test-integration.sh tests/dml                          # one suite
./scripts/test-integration.sh --core-image=...:latest -x         # override Core image
```

`runner.py` builds `firebolt-adbc-integration-test-runner:latest` locally on
first invocation (from `tests/integration/docker/Dockerfile`) — no registry
needed for the runner. The Firebolt Core image (`--core-image`, default in
`runner.py::DEFAULT_CORE_IMAGE`) is pulled if missing locally; on CI that pull
needs ECR auth (see [.github/workflows/enable-merge-to-main.yaml](.github/workflows/enable-merge-to-main.yaml)).

## Key Design Decisions

- **No packdb internal headers** — the `.so` must be loadable outside the server process.
- **No dependency discovery in CMake** — this project does not use `find_package`,
  `find_library`, `find_path`, `find_program`, or `FetchContent` for required deps.
  All non-system deps must exist under `adbc/submodule`.
- **nanoarrow** instead of packdb's `arrow_static` — `arrow_static` is compiled without
  `-fPIC` (it targets an executable) so it can't be linked into a shared library.
  nanoarrow is Apache Arrow's official embedded C implementation: zero external deps,
  fully PIC, ~30 KB compiled, supports the full Arrow IPC stream format.
- **Static third-party deps** — `curl`, `BoringSSL`, `c-ares`, `nanoarrow`, and test
  dependencies are linked statically from `adbc/submodule` build outputs. System runtime
  libs (e.g. `libc`, `libm`, `libdl`, `libpthread`) remain dynamic.
- **Version script** (`firebolt_adbc.version`) — exports only `AdbcDriverInit` and
  `FireboltAdbcDriverInit`; all other symbols (including libc++ internals) are hidden.
- **Post-build dependency report** — every build prints concise `DT_NEEDED` `.so` names
  for `libfirebolt_adbc.so` so dynamic dependencies are visible in Ninja logs.
- **SQL injection safety** — `quoteIdentifier()` in `FireboltAdbcDriver.cpp` wraps table
  names and column names in double quotes (embedding `"` doubled) before they are
  interpolated into auto-generated INSERT SQL.
- **Single source of truth for the Core image** — the registry string lives only in
  `tests/integration/runner.py::DEFAULT_CORE_IMAGE`, flows through the
  `FIREBOLT_CORE_IMAGE` env var into `helpers/firebolt_core.py::FireboltCore.__init__`,
  and is inlined into the generated docker-compose yaml at run time.
- **Generated test artifacts are gitignored** — every test run regenerates
  `tests/integration/_test_runtime_root/` (compose yaml, config json, container logs);
  the directory is in `.gitignore` so it is never committed.

## Dependencies

All required non-system deps are expected under `adbc/submodule`:

| Dep | Path |
|-----|---------------------|
| curl headers | `adbc/submodule/curl/include` |
| libcurl | `adbc/build/submodule/curl/lib/libcurl.a` |
| BoringSSL | `adbc/build/submodule/boringssl/libssl.a`, `libcrypto.a` |
| c-ares | `adbc/build/submodule/c-ares/src/lib/libcares.a` |
| nanoarrow source | `adbc/submodule/nanoarrow` |
| googletest source (tests) | `adbc/submodule/googletest` |

## ADBC Options Reference

| Key | Set on | Description |
|-----|--------|-------------|
| `"uri"` | Database | HTTP query endpoint, e.g. `http://localhost:3473` or `https://api.us-east-1.aws.app.firebolt.io` |
| `"adbc.firebolt.token"` | Database | JWT bearer token — omit for unauthenticated access |
| `"adbc.firebolt.database"` | Database | Database name (appended as `?database=…` query param) |
| `"adbc.firebolt.timeout_sec"` | Database | Query timeout in seconds (default 300) |
| `ADBC_INGEST_OPTION_TARGET_TABLE` | Statement | Target table for the bind-data ingest path; auto-generates `INSERT INTO {target} ({cols}) SELECT * FROM read_arrow('upload://data.arrow')` on `ExecuteUpdate` |
| Unknown keys on `ConnectionSetOption` after init | Connection | Stored as session params appended to query URL |

`ADBC_INGEST_OPTION_MODE`, `ADBC_INGEST_OPTION_TEMPORARY`, and the
catalog/schema target options are currently silently accepted by
`StatementSetOption` but not honoured. The high-level
`adbc_driver_manager.dbapi.Cursor.adbc_ingest()` does not yet land data in
the target table; the low-level `bind_stream` + `execute_update` path works
(see `tests/integration/tests/dml/test.py` for INSERT-via-SQL coverage).

## HTTP Protocol

- **SELECT / DDL**: `POST {url}?output_format=ArrowStream&database=...` with URL-encoded
  SQL body. Response is an Arrow IPC stream.
- **INSERT with bind data**: `POST {url}` as `multipart/form-data` — one `sql` part
  referencing `upload://data.arrow`, one `data.arrow` part with Arrow IPC bytes.
- **Session state**: updated via `Firebolt-Update-Parameters` / `Firebolt-Remove-Parameters`
  / `Firebolt-Reset-Session` response headers (see `src/HttpClient.cpp`).
