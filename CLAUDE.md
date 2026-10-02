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

# CLAUDE.md — Firebolt ADBC Driver

## Project Overview

A standalone C++ shared library (`libadbc_driver_firebolt.so`) implementing the
[ADBC 1.1.0](https://arrow.apache.org/adbc/) C API against Firebolt's HTTP query
interface. Client-side only — loaded at runtime by ADBC driver managers (Python
`adbc_driver_manager`, R `adbcdrivermanager`, etc.).

This repository ([`adbc-drivers/firebolt`](https://github.com/adbc-drivers/firebolt),
formerly `firebolt-db/firebolt-adbc`, whose URLs still redirect) is the driver and
nothing else: a **completely independent CMake project** with no coupling to the
packdb build system. It was extracted from `adbc/` in the packdb repo — paths in
this document are relative to *this* repository's root, not to packdb.

The repository is **public**: everything committed — code, tests, docs, commit
messages and PR text — is visible to anyone. Never commit secrets, credentials or
private keys, including test material: generate test certificates at run time.

## Directory layout

```
.
├── README.md                              # user-facing overview, quickstart, feature/type support
├── OPTIONS.md                             # full options reference (all three levels)
├── CHANGELOG.md                           # release history (Keep a Changelog)
├── CONTRIBUTING.md                        # build/test/release workflow for contributors
├── CLAUDE.md                              # this file
├── firebolt.toml                          # ADBC driver manifest (shipped with releases)
├── LICENSE.txt                            # Apache 2.0
├── NOTICE.txt                             # attribution notice
│
├── docs/
│   └── authentication.md                  # what auth works today + the SDK-spec target model
│
├── examples/
│   └── python/                            # runnable, env-var configured, no framework
│       ├── quickstart.py                  # connect + query, dbapi and low-level paths
│       ├── query_to_pandas.py             # pyarrow / pandas / polars + batch streaming
│       └── bulk_ingest.py                 # 4 ingest modes, nested ARRAY/STRUCT, bind_stream
│
├── CMakeLists.txt                         # standalone CMake project; project(VERSION) is authoritative
├── CMakePresets.json                      # presets: standalone-clang / standalone-gcc
├── adbc_driver_firebolt.version                  # linker version script (exports only AdbcDriverInit)
├── adbc.h                                 # vendored ADBC 1.1.0 C header + Arrow C ABI structs
├── submodule/                             # all non-system deps (git submodules + local static builds)
│
├── src/                                   # driver implementation
│   ├── FireboltAdbcDriver.cpp             # ADBC function table, entry point, curl init/cleanup
│   ├── FireboltAdbcDatabase.h             # FireboltDatabase struct (AdbcDatabase::private_data)
│   ├── FireboltAdbcConnection.h           # FireboltConnection struct; owns HttpClient + session state
│   ├── FireboltAdbcStatement.h            # FireboltStatement struct; SQL, bound Arrow batches, ingest target
│   ├── FireboltAdbcMetadata.h/.cpp        # GetInfo, GetTableTypes, GetTableSchema, GetObjects
│   ├── HttpClient.h/.cpp                  # libcurl wrapper: query POST, multipart insert, session headers
│   ├── HttpHeaderParse.h/.cpp             # session-state response header parsing
│   ├── IngestSqlBuilder.h/.cpp            # identifier quoting, Arrow→Firebolt types, ingest DDL/DML
│   ├── QueryParameters.h/.cpp             # bound Arrow row → `query_parameters` JSON ($1, $2, …)
│   ├── DescribeParameters.h/.cpp          # describe_parameters JSON → ADBC parameter schema
│   ├── ScopeGuard.h                       # RAII exit guard
│   ├── TlsConfig.h/.cpp                   # CA-bundle choice at DatabaseInit (option, SSL_CERT_FILE, distro paths)
│   ├── Version.h.in                       # → build/generated/Version.h; FIREBOLT_ADBC_VERSION
│   └── ArrowIpcStream.h/.cpp              # Arrow IPC bytes → ArrowArrayStream via nanoarrow 0.8.0
│
├── scripts/                               # locally runnable, idempotent — also called from CI
│   ├── build.sh                           # builds inside the pinned firebolt-adbc-builder Docker
│   │                                      #   image (Ubuntu 22.04 + clang-18) for glibc portability;
│   │                                      #   inits submodules, cmake + ninja, tests ON, SSL ON
│   ├── test-unit.sh                       # ctest --output-on-failure in build/
│   └── test-integration.sh                # forwards args to tests/integration/runner.py
│
├── docker/
│   └── builder/Dockerfile                 # the pinned build image scripts/build.sh uses
│
├── .github/
│   └── workflows/
│       ├── enable-merge-to-main.yaml      # build + unit + integration + examples on every PR
│       └── release.yaml                   # tag v* → multi-arch .so + sha256 + manifest on a release
│
└── tests/
    ├── unit/
    │   └── adbc_driver_test.cpp           # Google Test unit tests (no server needed)
    └── integration/                       # pytest harness — runs inside a runner container
        ├── runner.py                      # spins up runner image + 1-node Firebolt engine,
        │                                  #   exec's pytest. Args: --engine-image, --adbc-binary
        ├── conftest.py                    # shared fixtures: started_engine, server_url, conn,
        │                                  #   dbapi_conn, cursor, run_query, ingest, table_name,
        │                                  #   temp_table, scratch_table, mock_server
        ├── pytest.ini                     # python_files = test.py
        ├── docker/
        │   ├── Dockerfile                 # firebolt-adbc-integration-test-runner image
        │   └── requirements.txt           # adbc-driver-manager, pyarrow, pytest, requests
        ├── helpers/
        │   ├── __init__.py
        │   ├── mock_firebolt_server.py    # in-process HTTP(S) mock; https:// generates a throwaway cert
        │   └── firebolt_engine.py         # minimal docker-compose 1-node Firebolt engine fixture
        └── tests/                         # one directory per test area
            ├── adbc_sanity/test.py        # connectivity, literal selects, arithmetic, strings
            ├── advanced_queries/test.py   # joins, CTEs, CASE, HAVING, subqueries, window funcs
            ├── array_type/test.py         # ARRAY literals, roundtrip, functions, nested arrays
            ├── arrow/test.py              # schema, result shape, error handling, C-stream
            ├── datatypes/test.py          # int/float/string/bool/null/date/timestamp scalars
            ├── decimal_type/test.py       # DECIMAL literals, arithmetic, precision/scale
            ├── dml/test.py                # DDL, INSERT/SELECT, aggregates, type roundtrip
            ├── driver_loading/test.py     # entry-point names a driver manager resolves
            ├── ingest/test.py             # bulk ingest via dbapi Cursor.adbc_ingest()
            ├── ingest_low_level/test.py   # bulk ingest via set_options + bind_stream
            ├── prepared_statements/test.py # parameter schema; request budget via mock_server
            ├── query_params/test.py       # $N binding: types, values, executemany, param('name')
            ├── security_*/test.py         # regression tests for fixed security issues
            ├── tls/test.py                # https:// via the TLS mock: CA option, strict verification
            └── struct_type/test.py        # STRUCT / ARRAY(STRUCT) retrieval and ingest
```

## Build

### Recommended: `./scripts/build.sh`

Builds inside the pinned `firebolt-adbc-builder:latest` image (Ubuntu 22.04 +
clang-18), which it builds from `docker/builder/Dockerfile` on first use. The
older glibc is the point: the resulting `.so` needs only glibc 2.34, so it loads
on distributions older than the host. This is what CI and the release workflow
use, and it passes `-DFIREBOLT_ADBC_BUILD_TESTS=ON -DWITH_SSL=ON`.

```bash
./scripts/build.sh
# → build/libadbc_driver_firebolt.so -> libadbc_driver_firebolt.so.0 -> libadbc_driver_firebolt.so.0.1.0
```

### Direct host build (faster to iterate, not shippable)

Carries the host's glibc requirement, so never release the output.

```bash
git submodule update --init --recursive
cmake --preset standalone-clang -DFIREBOLT_ADBC_BUILD_TESTS=ON
cmake --build build -j$(nproc)
```

`standalone-gcc` is the same dependency policy with GCC. Note that a `build/`
directory configured on the host can break `scripts/build.sh` afterwards — a
`ccache` compiler launcher baked into `CMakeCache.txt` does not exist inside the
builder image. Delete `build/` and re-run.

## Testing

The reusable scripts under `scripts/` are the canonical entry points — both
local development and CI invoke the same commands.

```bash
# Build with -DFIREBOLT_ADBC_BUILD_TESTS=ON (idempotent):
./scripts/build.sh

# C++ unit tests (no server needed):
./scripts/test-unit.sh

# Integration tests against a 1-node Firebolt engine (Docker required):
./scripts/test-integration.sh                                    # all tests
./scripts/test-integration.sh -k test_connect                    # filter by name
./scripts/test-integration.sh tests/dml                          # one suite
./scripts/test-integration.sh --engine-image=...:latest -x       # override engine image
```

`runner.py` builds `firebolt-adbc-integration-test-runner:latest` locally on
first invocation (from `tests/integration/docker/Dockerfile`) — no registry
needed for the runner. The engine image (`--engine-image`, default in
`runner.py::DEFAULT_ENGINE_IMAGE`) is pulled from the public GHCR repo on every
run — it is a floating `:latest` tag, so a cached copy is only used as a
fallback when the pull fails.

The image ships the unified `firebolt` binary (server + client): its entrypoint
execs `firebolt <args>` and its default command is
`server --data-dir /var/lib/firebolt`. With no config file supplied the server
starts from its built-in structured (YAML) defaults — one node, all interfaces,
default ports — so the 1-node fixture writes no config at all. The legacy
`--node N` + `/firebolt-core/config.json` startup contract is gone; a multi-node
setup would bind-mount a `config.yaml` at `/var/lib/firebolt/config.yaml`.

## Key Design Decisions

- **No packdb internal headers** — the `.so` must be loadable outside the server process.
- **No dependency discovery in CMake** — this project does not use `find_package`,
  `find_library`, `find_path`, `find_program`, or `FetchContent` for required deps.
  All non-system deps must exist under `submodule`.
- **nanoarrow** instead of packdb's `arrow_static` — `arrow_static` is compiled without
  `-fPIC` (it targets an executable) so it can't be linked into a shared library.
  nanoarrow is Apache Arrow's official embedded C implementation: zero external deps,
  fully PIC, ~30 KB compiled, supports the full Arrow IPC stream format.
- **Static third-party deps** — `curl`, `BoringSSL`, `c-ares`, `nanoarrow`, and test
  dependencies are linked statically from `submodule` build outputs. System runtime
  libs (e.g. `libc`, `libm`, `libdl`, `libpthread`) remain dynamic.
- **JSON goes through `nlohmann/json`, not hand-rolled parsing** — two protocol
  surfaces are JSON: the `query_parameters` setting the driver writes and the
  `describe_parameters` payload it reads. The library owns escaping, UTF-8 validation
  and number formatting; it is header-only, so it costs nothing at link time, and it
  is the version packdb vendors. Not delegated: which JSON type each Arrow value
  becomes, since the server infers a parameter's SQL type from it.
- **Calendar arithmetic goes through C++20 `<chrono>`** — `QueryParameters.cpp` renders
  date, time and timestamp parameters as text. The error-prone parts (civil date from
  a day count, splitting an instant into day plus time of day, flooring rather than
  truncating before 1970) are `sys_days`, `year_month_day`, `hh_mm_ss` and
  `floor<days>`, available in both supported toolchains. Their *formatters* are not —
  libstdc++ gained those in 13, and the builder image has 11 — so digits go through
  `snprintf` and `withTimeUnit` dispatches an Arrow time unit to its duration type.
  Hence no vendored date library.
- **Version script** (`adbc_driver_firebolt.version`) — exports only `AdbcDriverInit` and
  `AdbcDriverFireboltInit`, the name the Foundry's shared-library rules derive from the
  driver name; all other symbols (including libc++ internals) are hidden, and nothing
  outside `Adbc*` is exported.
- **Post-build dependency report** — every build prints concise `DT_NEEDED` `.so` names
  for `libadbc_driver_firebolt.so` so dynamic dependencies are visible in Ninja logs.
- **SQL injection safety** — `quoteIdentifier()` in `IngestSqlBuilder.cpp` wraps table
  names, column names and struct field names in double quotes (embedding `"` doubled)
  before they are interpolated into auto-generated DDL/INSERT SQL.
- **Nested-type DDL follows Firebolt's nullability rules** — `arrowTypeToFireboltSqlType()`
  renders Arrow STRUCT as `STRUCT("field" TYPE, …)` and LIST/LARGE_LIST/FIXED_SIZE_LIST as
  `ARRAY(TYPE)`, recursing to any depth. A non-nullable Arrow *field* is widened to nullable
  because Firebolt rejects `STRUCT(… NOT NULL)` ("STRUCT fields have to be nullable"); `NOT
  NULL` is only emitted for a non-nullable top-level *column*. A zero-field struct maps to
  nothing (there is no `STRUCT()` in Firebolt), so ingest fails with
  `ADBC_STATUS_NOT_IMPLEMENTED` instead of uploading DDL the server would reject. MAP has no
  mapping either and fails the same way.
- **Single source of truth for the engine image** — the registry string lives only in
  `tests/integration/runner.py::DEFAULT_ENGINE_IMAGE`, flows through the
  `FIREBOLT_ENGINE_IMAGE` env var into
  `helpers/firebolt_engine.py::FireboltInstance.__init__`, and is inlined into the
  generated docker-compose yaml at run time.
- **Readiness is `/ping` *and* `SELECT 1`** — `/ping` turns green before the engine can
  serve queries ("Cluster not yet healthy"), so `FireboltInstance.start()` probes both
  before handing the URL to tests.
- **Generated test artifacts are gitignored** — every test run regenerates
  `tests/integration/_test_runtime_root/` (compose yaml, container logs);
  the directory is in `.gitignore` so it is never committed.
- **One version number** — `project(adbc_driver_firebolt VERSION …)` in `CMakeLists.txt` is
  authoritative. `configure_file` renders `src/Version.h.in` into
  `build/generated/Version.h`, whose `FIREBOLT_ADBC_VERSION` supplies
  `ADBC_INFO_DRIVER_VERSION`; the same value sets the target `VERSION`/`SOVERSION`.
  `release.yaml` refuses to publish when the git tag disagrees with it.
- **A bad option is an error, not a shrug** — an unrecognised `adbc.firebolt.*` database
  key returns `ADBC_STATUS_NOT_FOUND` (keys outside that namespace stay accepted, since
  the driver manager sets some itself), a malformed `timeout_sec` returns
  `ADBC_STATUS_INVALID_ARGUMENT` rather than throwing `std::invalid_argument` through the
  C ABI and aborting the host process, and `uri` is scheme-checked at `DatabaseInit`.
- **A rejected database option is reported by `DatabaseInit`, not by `DatabaseSetOption`** —
  see `RejectOption()`. This is a workaround for a heap overflow in the driver manager, not
  a style choice. A driver manager buffers options set before the driver is loaded and
  replays them from inside `AdbcDatabaseInit`; the failure path of that replay in
  adbc-driver-manager (through at least 1.8.0, `adbc_driver_manager.cc`
  `SetError(AdbcError*, AdbcError*)`) does
  `error->message = new char[strlen(src)]` and then writes the terminator at
  `[strlen(src)]` — one byte past the allocation, which aborts the process. Conventional
  drivers accept and discard unknown options, so nothing had exercised it. Returning the
  error from `Init` instead keeps the diagnostic; the manager forwards `Init`'s error
  struct through untouched. Options set *after* `Init` are refused immediately, since no
  replay is involved. Revisit if the upstream bug is fixed and the pinned version moves.
- **No exception may cross the C ABI** — the caller is a C driver manager with no handler,
  so anything that escapes aborts the host process. Every entry point that can throw
  wraps its body; `std::stol` and friends need explicit guards.
- **Binary size: dead code goes, speed stays** — TLS more than doubled the `.so` (2.5 →
  5.5 MB). The build now compiles everything, dependencies included, with
  `-ffunction-sections -fdata-sections` and links with `--gc-sections`; with only two
  exported symbols most of BoringSSL, curl and libstdc++ is unreachable (−1.65 MB). lld
  (`-fuse-ld=lld`, detected with `check_linker_flag`; `CMAKE_LINKER` is ignored by the
  compiler driver, so it was never in effect) adds `--icf=all`. curl drops features the
  driver never calls (`CURL_DISABLE_HTTP_AUTH` and the other auth schemes, HSTS, alt-svc,
  netrc, the deprecated form API — not MIME, which ingest uses), and BoringSSL builds with
  `OPENSSL_SMALL`, whose only cost is a slightly slower TLS handshake. Result: 3.45 MB.
  Deliberately **not** done: `-Os`, which would save another 0.5 MB by slowing the Arrow
  and JSON hot paths, and stripping the symbol table, which would cost readable crash
  stacks.
- **The CA bundle is chosen at run time, never compiled in** — curl's configure step
  records the build machine's bundle path, which names the Ubuntu builder image's layout
  and is wrong on RHEL, Amazon Linux or SUSE. The build sets `CURL_CA_BUNDLE`/`CURL_CA_PATH`
  to `none`, and `DatabaseInit` resolves one (`TlsConfig.cpp`):
  `adbc.firebolt.ssl_certificate_path`, then `SSL_CERT_FILE`, then the standard distro
  paths, handed to `CURLOPT_CAINFO`. A configured source that is unreadable is an error,
  not a fall-through. Verification has no off switch.
- **TLS capability is asked of libcurl, not tracked in a define** — `curl_version_info`
  reports whether the linked curl has SSL, so the `https://` rejection is always correct
  for the library actually loaded rather than for what the build flags claimed.
- **Query parameters ride the same channel as session settings** — binding sends the
  values in the `query_parameters` query setting, a JSON array of
  `{"name": "$1", "value": …}`, and the server substitutes each `$N` into the *parsed*
  statement (`SqlExprValidator::_visit_parameter` in packdb). Nothing is spliced into
  SQL text, so the only escaping that matters is JSON escaping. This is the mechanism
  packdb's own PostgreSQL-wire handler and the `fb` CLI's `--param` both use. Bound
  data serves two paths, told apart by whether an ingest target table is set: with one
  it is a multipart ingest payload, without one it is query parameters, one execution
  per bound row. Since that option decides the destination, ingest options arriving
  *without* a target table are refused at execute time — checked there, not where the
  options arrive, since ADBC does not order option calls.
- **Bind keeps Arrow arrays; only ingest encodes IPC** — `BoundData` holds the bound
  `ArrowArray` batches, and `SerializeBoundData` runs in the ingest branch of
  `StatementExecuteQuery`, the only consumer of the bytes. Encoding at bind time would
  cost every parameterised execution an encode plus decode of data that leaves as
  JSON, and would refuse any layout nanoarrow's IPC writer cannot encode — Arrow's
  view layouts among them. `BindStream` drains its stream on the spot, a stream being
  single-pass. "Data was supplied" is `data_bound`, not a non-empty batch list, so a
  zero-batch ingest still uploads a schema-only payload and creates an empty table.
- **A parameter's SQL type is the JSON type of its value** — the format has no type
  field, so `int`→BIGINT, JSON number→DOUBLE, bool→BOOLEAN, `null`→untyped NULL,
  string→TEXT. Hence `QueryParameters.cpp` keeps a `.0` on whole doubles (a bare `3`
  would arrive as a BIGINT), sends decimals as strings (a JSON number is read back
  through `std::stod`, which rounds), and refuses `binary`/nested types by name
  instead of sending something the server would misread.
- **Prepare issues no request; GetParameterSchema does** — there is no server-side
  prepare, and ADBC makes `Prepare` optional, so it stays local: driver managers call
  it on every query-text change, and a round-trip there would double the request count
  of every `execute()`. Parameter types come from re-sending the statement with
  `execution_mode=describe_parameters`, minus the
  `transaction_id`/`transaction_sequence_id` session parameters, so type inference
  never spends a transaction step on a statement the caller never ran.
- **Nothing about a statement is cached across executions** — neither the describe
  payload nor a bound payload. DDL from any connection changes a placeholder's type
  without changing a character of the query, and a driver manager re-issues
  `SetSqlQuery` only on a text change (dbapi skips it when `operation ==
  self._last_query`), so a cache keyed on the SQL would outlive its truth with no
  invalidation event the driver can see.
- **A statement option a driver manager may leave stale must only ever add behaviour**
  — `bind_by_name` is the case in point: dbapi sends it once per cursor and never
  revises it when parameters arrive as Arrow data, so it cannot be trusted to describe
  the current call. `buildQueryParametersJson` therefore always emits the positional
  `$N` names and treats the option as a request for *extra* aliases, making a stale
  `true` cost one unreferenced parameter rather than unbinding every placeholder.

## Dependencies

All required non-system deps are expected under `submodule`:

| Dep | Path |
|-----|---------------------|
| curl headers | `submodule/curl/include` |
| libcurl | `build/submodule/curl/lib/libcurl.a` |
| BoringSSL | `build/submodule/boringssl/libssl.a`, `libcrypto.a` |
| c-ares | `build/submodule/c-ares/src/lib/libcares.a` |
| nanoarrow source | `submodule/nanoarrow` |
| nlohmann/json (header-only) | `submodule/json/single_include` |
| googletest source (tests) | `submodule/googletest` |

`nlohmann/json` is pinned to `v3.12.0`, the version packdb vendors, and included as a
SYSTEM directory so its headers are exempt from this project's warnings. Header-only:
nothing is added to the link line.

## ADBC Options Reference

**[OPTIONS.md](OPTIONS.md) is the authoritative reference** — every option, every
error status, and the full Arrow→Firebolt type mapping. Summary only here.

| Key | Set on | Description |
|-----|--------|-------------|
| `"uri"` | Database | HTTP query endpoint, e.g. `http://localhost:3473`. Scheme-validated at `Init`; `https://` verifies the peer against the CA bundle chosen at `Init`. |
| `"adbc.firebolt.token"` | Database, Connection | Bearer token — omit for an auth-disabled engine. Per-connection when set on the connection. Contradicts the SDK auth spec (a raw JWT belongs in `FIREBOLT_TOKEN`) and will be removed; see `docs/authentication.md`. |
| `"adbc.firebolt.database"` | Database | Database name (appended as `?database=…` query param) |
| `"adbc.firebolt.ssl_certificate_path"` | Database | PEM CA bundle for `https://`; default is `SSL_CERT_FILE`, then the distro bundle paths |
| `"adbc.firebolt.timeout_sec"` | Database | Total request timeout in whole seconds; `0` (the default) disables it |
| `ADBC_CONNECTION_OPTION_AUTOCOMMIT` | Connection | `false` enables explicit transactions: lazy `BEGIN`, then `Commit`/`Rollback` |
| `ADBC_INGEST_OPTION_TARGET_TABLE` | Statement | Target table for the bind-data ingest path; auto-generates `INSERT INTO {target} ({cols}) SELECT * FROM read_arrow('upload://data.arrow')` on `ExecuteUpdate` |
| `"adbc.statement.bind_by_name"` | Statement | `true` additionally names bound parameters after their columns (for `param('name')`); the positional `$N` names are always sent too, so a stale setting cannot unbind a placeholder. Only the canonical `true`/`false` accepted. Absent from the vendored ADBC 1.1.0 header, so defined locally |
| Unknown keys on `ConnectionSetOption` after init | Connection | Stored as session params appended to query URL |

`ADBC_INGEST_OPTION_MODE` (append / create / replace / create_append) and the
catalog/schema target options are honoured: the create-style modes synthesise
`CREATE TABLE` DDL from the bound Arrow schema, including nested
`STRUCT`/`ARRAY` columns. `ADBC_INGEST_OPTION_TEMPORARY` is rejected with
`ADBC_STATUS_NOT_IMPLEMENTED` — Firebolt has no session-temporary tables. Both
the high-level `adbc_driver_manager.dbapi.Cursor.adbc_ingest()` and the
low-level `bind_stream` + `execute_update` path land data in the target table
(`tests/integration/tests/ingest/`, `ingest_low_level/`, `struct_type/`;
`dml/test.py` covers INSERT-via-SQL).

Query parameters use Firebolt's positional `$1`, `$2`, … placeholders — not `?` or
`%s` — carried in the `query_parameters` query setting. `StatementGetParameterSchema`
reports the types the server infers via `execution_mode=describe_parameters`
(`tests/integration/tests/query_params/`, `prepared_statements/`).

## HTTP Protocol

- **SELECT / DDL**: `POST {url}?output_format=ArrowStream&database=...` with URL-encoded
  SQL body. Response is an Arrow IPC stream. Sends `Firebolt-Protocol-Version: 2.4`.
- **INSERT with bind data**: `POST {url}` as `multipart/form-data` — one `sql` part
  referencing `upload://data.arrow`, one `data.arrow` part with Arrow IPC bytes.
- **Session state**: updated via `Firebolt-Update-Parameters` / `Firebolt-Remove-Parameters`
  / `Firebolt-Reset-Session` response headers (see `src/HttpClient.cpp`). Applied only on a
  successful response — a 4xx/5xx body may come from a proxy or an attacker.

## Authentication: where this driver stands

**Do not infer Firebolt's auth model from this driver's options, and do not carry over
the Firebolt SaaS 2.0 model.** There are no service accounts, no `account_name`, no
`api_endpoint`, and no control-plane engine resolution; those are explicitly legacy.

The authoritative specs live in the packdb repo, and `specs/sdk-authentication.md` names
ADBC directly. In short: a client discovers everything from
`GET <host>/.well-known/firebolt` (`instance.auth` is `null` for auth-disabled, else it
carries `oauth.protectedResource` plus `authorizationServers[]` and an optional
`preferredAuthorizationServer`), then runs OAuth 2.0 `client_credentials` against the
selected server's `token_endpoint` with `username`→`client_id`, `password`→`client_secret`
and the RFC 8707 `resource` bound to the instance. Token precedence is `FIREBOLT_TOKEN`
(one-shot, never cached) > connection credentials > optionally `firebolt token <host>`.
Transport is a separate `ssl_mode` parameter defaulting to `verify-full`.

This driver implements **none** of that yet: no discovery, no `client_credentials`, no
`FIREBOLT_TOKEN`, no `ssl_mode` (TLS ships, always `verify-full`), and canonical parameter
names (`host`, `database`, `query_timeout`, … per
`specs/schemas/connection-parameters.v1.json`) not yet adopted. `docs/authentication.md`
documents the gap for users; the rename, when it happens, replaces the current
`adbc.firebolt.*` keys outright rather than aliasing them — nothing external consumes
them yet.
