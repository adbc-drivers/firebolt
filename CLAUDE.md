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
├── STATUS.md                              # implementation status & changelog
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
│   └── ArrowIpcStream.h/.cpp             # Arrow IPC bytes → ArrowArrayStream via nanoarrow 0.8.0
│
├── examples/
│   ├── python/
│   │   └── demo_firebolt_adbc.py          # Python demo (low-level bind + DBAPI adbc_ingest)
│   └── go/
│       └── demo_firebolt_adbc.go          # Go demo (same scenarios, uses arrow-adbc/go)
│
└── tests/
    ├── unit/
    │   └── adbc_driver_test.cpp           # Google Test unit tests (12 tests, no server needed)
    └── integration/
        ├── python/
        │   ├── conftest.py                # pytest fixtures: server lifecycle, conn, dbapi_cursor
        │   ├── requirements.txt           # adbc-driver-manager, pyarrow, pytest, requests
        │   ├── test_basic.py              # connection, simple SELECT queries
        │   ├── test_arrow.py              # Arrow result column types
        │   ├── test_datatypes.py          # scalar type round-trips
        │   ├── test_decimal.py            # DECIMAL precision / scale edge cases
        │   ├── test_dml.py                # INSERT / UPDATE / DELETE
        │   ├── test_transactions.py       # BEGIN / COMMIT / ROLLBACK
        │   ├── test_metadata.py           # GetInfo, GetTableTypes, GetTableSchema, GetObjects
        │   ├── test_advanced_queries.py   # joins, aggregates, window functions
        │   ├── test_array.py              # ARRAY type support
        │   ├── test_struct.py             # STRUCT type support and deep nesting
        │   └── test_batch_insert.py       # Arrow IPC batch ingest (38 tests)
        └── go/
            ├── helpers_test.go            # TestMain, newConn, ddl, scalar*, selectAll, bindInsert, ingest
            └── batch_insert_test.go       # 35 tests mirroring the Python batch insert suite
```

## Build

### Recommended (clang-18 + adbc/submodule deps)

```bash
# From the packdb repo root:
git submodule update --init --recursive adbc/submodule
cmake --preset standalone-clang -S adbc
cmake --build adbc/build -j$(nproc)
# → adbc/build/libfirebolt_adbc.so
```

### With tests

```bash
cmake --preset standalone-clang -S adbc -DFIREBOLT_ADBC_BUILD_TESTS=ON
cmake --build adbc/build -j$(nproc)
cd adbc/build && ctest --output-on-failure
```

### GCC preset (same dependency policy)

```bash
cmake --preset standalone-gcc -S adbc
cmake --build adbc/build -j$(nproc)
```

## Testing

```bash
# C++ unit tests (no server needed):
cd adbc/build && ctest --output-on-failure

# Python integration tests:
cd adbc/tests/integration/python
pytest --no-docker -v          # server already running
pytest -v                      # let pytest start Docker

# Go integration tests:
cd adbc/tests/integration/go
go test -v ./...
```

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
| Unknown keys on `ConnectionSetOption` after init | Connection | Stored as session params appended to query URL |

## HTTP Protocol

- **SELECT / DDL**: `POST {url}?output_format=ArrowStream&database=...` with URL-encoded
  SQL body. Response is an Arrow IPC stream.
- **INSERT with bind data**: `POST {url}` as `multipart/form-data` — one `sql` part
  referencing `upload://data.arrow`, one `data.arrow` part with Arrow IPC bytes.
- **Session state**: updated via `Firebolt-Update-Parameters` / `Firebolt-Remove-Parameters`
  / `Firebolt-Reset-Session` response headers (see `src/HttpClient.cpp`).
