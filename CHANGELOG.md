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

# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **TLS.** Released builds now link BoringSSL, so `https://` endpoints work. The peer is
  always verified. The CA bundle is found at run time — `SSL_CERT_FILE`, then the
  Debian, RHEL and SUSE locations — or named with the new
  `firebolt.ssl_certificate_path` option. A missing bundle fails `AdbcDatabaseInit`
  with a message naming every path tried.

### Changed

- **Smaller binary.** Unused code in the statically linked dependencies is now dropped
  at link time, and unused curl features are compiled out, so the TLS build is 3.45 MB on
  x86_64 instead of the 5.47 MB it would otherwise be (0.1.1 without TLS: 2.52 MB).
- **The driver-specific entry point is now `AdbcDriverFireboltInit`**, the name a driver
  manager derives from the driver name. `FireboltAdbcDriverInit` is no longer exported
  (only `Adbc*` symbols are); a manifest with `entrypoint = "FireboltAdbcDriverInit"`
  must switch to the new name or drop the line — `AdbcDriverInit` still works.
- **The library is `libadbc_driver_firebolt.so`** (was `libfirebolt_adbc.so`), the
  Foundry file name from which a driver manager derives `AdbcDriverFireboltInit`. Releases
  ship one `adbc_driver_firebolt-linux-<arch>.tar.gz` per architecture holding the
  library, `LICENSE.txt` and `NOTICE.txt`, instead of a renamed `.so` per architecture.
- **Driver options are named `firebolt.*`** (`firebolt.token`, `firebolt.database`,
  `firebolt.timeout_sec`, `firebolt.ssl_certificate_path`) instead of `adbc.firebolt.*`,
  per the Foundry option-naming rule.

## [0.1.1] - 2026-08-11

### Added

- **Query parameter binding.** `cursor.execute(sql, params)` and
  `cursor.executemany(sql, rows)` bind `$1`, `$2`, … through the `query_parameters`
  setting: server-side substitution, one request per row. Scalars keep their type
  (`BIGINT`/`DOUBLE`/`BOOLEAN`/untyped `NULL`; dates, timestamps and decimals as exact
  text); `binary` and nested types return `ADBC_STATUS_NOT_IMPLEMENTED`.
- **Named parameter binding** via `adbc.statement.bind_by_name`, set by Python dbapi for
  dict params: names map to `param('name')` as `TEXT`, and positional `$N` names are
  always sent too, so a stale option cannot leave placeholders unbound.
- **`AdbcStatementGetParameterSchema`** (`cursor.adbc_prepare`) reports the types from
  `execution_mode=describe_parameters`, without executing or caching, and without the
  transaction session params. `AdbcStatementPrepare` issues no request.

### Fixed

- Bound parameters no longer round-trip through Arrow IPC: only the ingest path encodes
  IPC, at execution. Saves an encode plus decode per parameterised execution, and
  `string_view` columns can now be bound (ingest still cannot take them).
- Ingest options set without `adbc.ingest.target_table` now fail with
  `ADBC_STATUS_INVALID_STATE` instead of being discarded. The target table routes bound
  data to the ingest path, so a missing or misspelled one turned an ingest into one
  parameterised execution of the caller's SQL per bound row.
- `adbc.ingest.mode` now defaults to `adbc.ingest.mode.create`, as ADBC and
  [OPTIONS.md](OPTIONS.md) specify, rather than to append. Reachable only through the
  low-level API — `Cursor.adbc_ingest()` always sends a mode — where an ingest without
  one failed with "table does not exist" instead of creating the table.
- Executing a bulk ingest overwrote the statement's own SQL text with the generated
  `INSERT`, leaving the statement unusable for re-execution.

## [0.1.0]

First release. The driver already implemented queries, bulk ingest, metadata, and
transactions.

### Known limitations

See [README.md](README.md#supported-today). In short: plaintext `http://` only
(no TLS in this build), authentication-disabled engines plus an optional
pre-obtained bearer token, no discovery-based OAuth, Linux only, and no
parameterized queries.

[Unreleased]: https://github.com/firebolt-db/firebolt-adbc/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/firebolt-db/firebolt-adbc/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/firebolt-db/firebolt-adbc/releases/tag/v0.1.0
