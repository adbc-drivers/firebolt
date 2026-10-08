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

# AGENTS.md — Firebolt ADBC Driver

## Project Overview

A standalone C++ shared library (`libadbc_driver_firebolt.so`, `.dylib`, or
`.dll`) implementing the [ADBC 1.1.0](https://arrow.apache.org/adbc/) C API
against Firebolt's HTTP query interface. Client-side only — loaded at runtime by
ADBC driver managers (Python `adbc_driver_manager`, R `adbcdrivermanager`, etc.).

This is an independent CMake project, extracted from `adbc/` in the packdb repo.
The repository ([`adbc-drivers/firebolt`](https://github.com/adbc-drivers/firebolt))
is **public**: never commit secrets, credentials or private keys, including test
material — generate test certificates at run time.

## Directory layout

```
.
├── CMakeLists.txt                    # standalone build; project(VERSION) is the one version number
├── adbc.h                            # vendored ADBC 1.1.0 C API header
├── adbc_driver_firebolt.{version,exports}  # Linux/macOS linker export allowlists
├── submodule/                        # every non-system dependency (curl, BoringSSL, nanoarrow, json, …)
├── src/                              # the driver
│   ├── FireboltAdbcDriver.cpp        # ADBC entry points: database, connection, statement
│   ├── HttpClient.*                  # libcurl wrapper for Firebolt's HTTP query protocol
│   └── fb2/                          # FB2 SaaS (Legacy) mode, isolated behind hooks
├── tests/
│   ├── unit/                         # GoogleTest, no server needed
│   └── integration/                  # pytest in a runner container: a 1-node engine,
│                                     #   a mock server, or an FB2 SaaS engine
├── validation/                       # shared ADBC validation suite adapter (uses compose.yaml)
├── ci/scripts/                       # adbc-make build, test, and license hooks
├── scripts/clang-tidy.sh             # clang-tidy against the generated compile database
└── docs/, examples/python/           # user docs and runnable examples
```

User docs: `README.md`, `OPTIONS.md` (the authoritative option, error and type
reference), `docs/authentication.md`, `docs/fb2-saas.md`.

## Build and test

```bash
git submodule update --init --recursive              # once, before any build
pixi run make                                        # release build → build/libadbc_driver_firebolt.*

# Debug build with C++ unit tests (adjust platform/arch). On Linux the build runs
# in the manylinux image and test.sh also runs clang-tidy and the integration suite:
./ci/scripts/build.sh test macos arm64
./ci/scripts/test.sh macos arm64

# Integration tests against a 1-node Firebolt engine (Docker required):
./tests/integration/runner.py                        # all tests
./tests/integration/runner.py -k test_connect        # filter by name
./tests/integration/runner.py tests/dml              # one suite
./tests/integration/runner.py --engine-image=...:tag -x

# Linters (clang-tidy needs a generated Linux test build):
pre-commit run --all-files
./scripts/clang-tidy.sh
```

`runner.py` builds its runner image locally on first use. The engine image is
pulled on every run (a cached copy is the fallback); its registry string lives
only in `tests/integration/runner.py::DEFAULT_ENGINE_IMAGE`. Readiness is `/ping`
*and* `SELECT 1` — `/ping` turns green before the engine can serve queries.

## Code style and naming conventions

Beyond what pre-commit and clang-tidy enforce:

- Descriptive names: no one-letter or vague names (`s`, `get()`, `perform()`); don't repeat the namespace in a name.
- No hand-written parsing: URLs via `parseUrl()`/`parseFireboltUri()` (libcurl), JSON via nlohmann/json, IPs via `inet_pton`.
- Use library calls, not inline lambdas; wrap a long call in a named helper in the anonymous namespace.
- Look a value up once: `if (auto * x = find(k); !x || x->empty())`.
- One `firebolt::adbc::Status` for a status code and message.
- `NOLINT` only with a reason; don't reformat lines you are not changing; no build switches for features.

## Key Design Decisions

### Build and dependencies

- **No packdb internal headers** — the library must load outside the server process.
- **No dependency discovery in CMake** — no `find_package`, `find_library`,
  `find_path`, `find_program`, or `FetchContent` for required deps. Every non-system
  dep lives under `submodule/` and is linked statically: curl, nanoarrow,
  nlohmann/json (header-only, pinned to packdb's `v3.12.0`), googletest, and
  BoringSSL on Linux and macOS (Windows uses Schannel). System runtime libraries
  stay dynamic.
- **No Linux `.so` is built against the host's glibc** — on Linux, `ci/scripts/build.sh`
  re-runs itself in `adbc-drivers/dev`'s `manylinux-cpp` compose service (the
  release image), with the repository mounted at its own path so ctest, clang-tidy
  and the integration runner use the build tree from the host. Inside the image
  (`AUDITWHEEL_PLAT` is set) it builds directly. Debug and test builds included.
- **nanoarrow, not Arrow C++** — zero deps, PIC, and full Arrow IPC stream support.
- **Export allowlists** — only `AdbcDriverInit` and `AdbcDriverFireboltInit` are
  exported (`.version`/`.exports` files; `__declspec(dllexport)` on Windows).
- **Binary size: dead code goes, speed stays** — `-ffunction-sections
  -fdata-sections` + `--gc-sections`, lld with `--icf=all`, curl features the driver
  never calls disabled, BoringSSL with `OPENSSL_SMALL`. Deliberately **not** `-Os`
  (slows the Arrow/JSON hot paths) and not stripped (keeps readable crash stacks).
- **One version number** — `project(adbc_driver_firebolt VERSION …)` in
  `CMakeLists.txt` feeds `ADBC_INFO_DRIVER_VERSION` (via `src/Version.h.in`) and the
  library `VERSION`/`SOVERSION`; the release workflow refuses a mismatching tag.

### Network and TLS

- **DNS goes through the OS** — curl's threaded resolver calls `getaddrinfo`, so
  nsswitch, systemd-resolved and VPN split DNS work. Don't reintroduce c-ares.
- **The CA bundle is chosen at run time, never compiled in** — `TlsConfig.cpp`
  resolves `firebolt.ssl_certificate_path`, then `SSL_CERT_FILE`, then the standard
  distro paths. An unreadable configured source is an error, not a fall-through.
  Verification has no off switch.
- **TLS capability is asked of libcurl** (`curl_version_info`), not tracked in a define.

### C ABI safety and options

- **No exception may cross the C ABI** — the caller is a C driver manager, so an
  escaping exception aborts the host process. Every entry point that can throw wraps
  its body; `std::stol` and friends need explicit guards.
- **A bad option is an error** — an unrecognised `firebolt.*` database key returns
  `ADBC_STATUS_NOT_FOUND`, other unsupported options `ADBC_STATUS_NOT_IMPLEMENTED`;
  only `firebolt.session.*` connection options are forwarded as server settings.
- **A rejected database option is reported by `DatabaseInit`, not
  `DatabaseSetOption`** (`RejectOption()`). This works around a heap overflow in
  adbc-driver-manager (seen through at least 1.8.0): when it replays buffered options
  inside `AdbcDatabaseInit` and one fails, its `SetError` copies the message one byte
  past its allocation and aborts. Options set *after* `Init` are refused immediately.
  Revisit if the upstream bug is fixed.

### Ingest and query parameters

- **SQL injection safety** — `quoteIdentifier()` in `IngestSqlBuilder.cpp` quotes
  every table, column and struct field name in generated DDL/INSERT SQL.
- **Nested-type DDL follows Firebolt's nullability rules** — STRUCT fields are
  always nullable (Firebolt rejects `STRUCT(… NOT NULL)`); `NOT NULL` only on
  top-level columns. Zero-field structs and MAP have no mapping and fail with
  `ADBC_STATUS_NOT_IMPLEMENTED` before anything is uploaded.
- **Query parameters ride the `query_parameters` setting** — a JSON array of
  `{"name": "$1", "value": …}`; the server substitutes into the *parsed* statement,
  so nothing is spliced into SQL and only JSON escaping matters. Bound data with an
  ingest target table is a multipart ingest payload; without one it is query
  parameters, one execution per row. Ingest options without a target table are
  refused at execute time.
- **Bind keeps Arrow arrays; only ingest encodes IPC** — `BoundData` holds the
  batches and `SerializeBoundData` runs only in the ingest branch. `BindStream`
  drains its stream immediately. `data_bound`, not a non-empty batch list, means
  "data was supplied", so a zero-batch ingest still creates an empty table.
- **A parameter's SQL type is the JSON type of its value** — `int`→BIGINT,
  number→DOUBLE, bool→BOOLEAN, `null`→NULL, string→TEXT. So whole doubles keep a
  `.0`, decimals go as strings, and binary/nested types are refused by name.
  Dates and times are rendered with C++20 `<chrono>` arithmetic and `snprintf`
  (libstdc++ 11 in the builder has no chrono formatters).
- **Prepare issues no request; GetParameterSchema does** — via
  `execution_mode=describe_parameters`, without the transaction session parameters
  so it never spends a transaction step.
- **Nothing about a statement is cached across executions** — DDL can change a
  placeholder's type without changing the query text, and there is no invalidation
  event.
- **A statement option a driver manager may leave stale must only add behaviour** —
  e.g. `bind_by_name`: positional `$N` names are always sent, and `true` only adds
  name aliases.

### FB2 SaaS (Legacy) mode

Support for Firebolt 2.0 SaaS engines (v5+): service-account credentials plus
`firebolt.account`/`engine`, exchanged for a token and resolved to an engine URL
through the 2.0 control plane. All of it lives in `src/fb2/` under
`firebolt::adbc::fb2` with a `// FB2 SaaS (Legacy) mode` banner. The main code holds
one optional `std::shared_ptr<Fb2LegacyMode>` (null on the Core path) and calls it
only at sites marked `FB2 SaaS (Legacy) mode hook`: option claim, `DatabaseInit`, the
bearer token and the one 401 retry, and the response hook. Always built — no switch.
Do not add FB2 behaviour outside the directory; add a hook. User docs for it live in
`docs/fb2-saas.md`; the README mentions it only in the "Supported today" table
(SaaS, v5+ engines only), the "Connect to Firebolt SaaS" quickstart example and
under "Other environments".

## HTTP Protocol

- **SELECT / DDL**: `POST {url}?output_format=ArrowStream&database=...` with the SQL
  as the body; the response is an Arrow IPC stream. Sends `Firebolt-Protocol-Version: 2.4`.
- **INSERT with bind data**: `multipart/form-data` — a `sql` part referencing
  `upload://data.arrow` plus a `data.arrow` part with Arrow IPC bytes.
- **Session state**: `Firebolt-Update-Parameters` / `Firebolt-Remove-Parameters` /
  `Firebolt-Reset-Session` response headers, applied only on a successful response.

## Authentication (Firebolt Core)

Don't model Core auth on this driver's options or on Firebolt 2.0 SaaS — service
accounts, `account_name` and control-plane engine resolution belong to FB2 mode only.
The authoritative spec is packdb's `specs/sdk-authentication.md`: discovery via
`GET <host>/.well-known/firebolt`, then OAuth 2.0 `client_credentials` with an RFC
8707 `resource`; token precedence `FIREBOLT_TOKEN` > connection credentials >
`firebolt token <host>`; transport via `ssl_mode` (default `verify-full`).

The driver implements none of that yet — only auth-disabled engines and a raw
`firebolt.token`. The canonical parameter names
(`specs/schemas/connection-parameters.v1.json`) are not adopted yet either; when they
are, they replace the `firebolt.*` keys outright rather than aliasing them.
`docs/authentication.md` documents the gap for users.
