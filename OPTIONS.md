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

# Options reference

Every option this driver reads, at all three ADBC levels. Anything not listed
here is not consumed by the driver — see [Unknown options](#unknown-options) for
what happens to it.

Options are set through whichever API your driver manager exposes. In Python:

```python
# Database-level options
dbapi.connect(driver=DRIVER, db_kwargs={"uri": ..., "firebolt.database": ...})

# Connection-level options
conn.adbc_connection.set_options(**{"firebolt.token": "..."})

# Statement-level options
stmt.set_options(**{"adbc.ingest.target_table": "events"})
```

---

## Database options

Set on `AdbcDatabase` before `AdbcDatabaseInit` (`db_kwargs` in Python).

| Key | Required | Default | Meaning |
|-----|----------|---------|---------|
| `uri` | **yes** | — | Where the engine is: a `firebolt://` URI (below) or its HTTP endpoint, e.g. `http://localhost:3473`. Validated at `Init` (`https://`, and the `firebolt://` default, need a TLS build, which is what ships). An `http(s)://` endpoint is used verbatim, so a path is preserved (`http://host/query` stays `/query`) and query parameters are appended to whatever is already there. |
| `firebolt.token` | no | none | Bearer token sent as `Authorization: Bearer <token>`. Omit it entirely for an engine with authentication disabled — no header is sent. See [docs/authentication.md](docs/authentication.md); this key is a stopgap and is going away. |
| `firebolt.database` | no | server default | Database name, appended to every request URL as `database=<value>`. |
| `firebolt.ssl_certificate_path` | no | system bundle | PEM file of CA certificates that verify an `https://` peer. Without it the driver uses `SSL_CERT_FILE` if set, else the first of `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem`, `/etc/ssl/cert.pem` that exists. Verification cannot be switched off. |
| `firebolt.timeout_sec` | no | `0` | Whole seconds; the total request timeout (libcurl `CURLOPT_TIMEOUT`). `0` disables it. Rejected with `ADBC_STATUS_INVALID_ARGUMENT` if not a non-negative integer. |

### `firebolt://` URIs

```
firebolt://<host>[:<port>]/[<database>][?ssl_mode=<mode>]
```

The shape of Firebolt's SDK connection string. `Init` resolves it to the HTTP
endpoint every request goes to:

| Part | Meaning |
|------|---------|
| `<host>[:<port>]` | The engine. IPv6 literals go in brackets: `firebolt://[::1]:3473`. |
| `/<database>` | Optional database name, percent-decoded (`my%20db`). One path segment only. `firebolt.database`, when also set, takes precedence. |
| `ssl_mode=verify-full` | The default: `https://`, verifying the certificate chain and host name. Needs a build with TLS. |
| `ssl_mode=disable` | Plaintext `http://`. For a local engine with authentication disabled. |

So `firebolt://localhost:3473/playground?ssl_mode=disable` is
`http://localhost:3473` with `firebolt.database=playground`.

Errors:

| Situation | Status |
|-----------|--------|
| `uri` missing at `Init` | `ADBC_STATUS_INVALID_ARGUMENT` |
| `uri` has no scheme, or a scheme other than firebolt/http/https | `ADBC_STATUS_INVALID_ARGUMENT` |
| `firebolt://` URI with no host, an invalid port, a nested path, other malformed syntax, or an unknown `ssl_mode` | `ADBC_STATUS_INVALID_ARGUMENT` |
| `firebolt://` URI with `ssl_mode=verify-ca` or `require` (verification cannot be relaxed) | `ADBC_STATUS_NOT_IMPLEMENTED` |
| `firebolt://` URI with credentials (`user:password@`) — any raw `@` counts; write a literal one as `%40` | `ADBC_STATUS_NOT_IMPLEMENTED` |
| `firebolt://` URI with a query parameter other than `ssl_mode` | `ADBC_STATUS_NOT_FOUND` |
| `uri` is `https://` on a build without TLS | `ADBC_STATUS_INVALID_ARGUMENT` |
| `firebolt.ssl_certificate_path` or `SSL_CERT_FILE` names a file that is not readable | `ADBC_STATUS_INVALID_ARGUMENT` |
| `uri` is `https://` and no system CA bundle exists | `ADBC_STATUS_INVALID_STATE` |
| `uri` or `firebolt.ssl_certificate_path` set after `Init` | `ADBC_STATUS_INVALID_STATE`, at once |
| `firebolt.timeout_sec` not a non-negative integer | `ADBC_STATUS_INVALID_ARGUMENT` |
| unknown `firebolt.*` key | `ADBC_STATUS_NOT_FOUND` |

All of these surface when you **open** the database, not from the individual
option call — so in Python they are raised by `dbapi.connect(...)`, which is where
you passed `db_kwargs` anyway. (The reason is in
[CLAUDE.md](CLAUDE.md): the driver manager replays pre-`Init` options from inside
`AdbcDatabaseInit`, and its failure path there has a one-byte heap overflow that
aborts the process, so the driver keeps out of it.) An option set *after* the
database is open is refused on the spot.

## Connection options

Set on `AdbcConnection`, before or after `AdbcConnectionInit`.

| Key | Default | Meaning |
|-----|---------|---------|
| `adbc.connection.autocommit` | `true` | `false` starts explicit transactions: the driver issues `BEGIN` lazily before the first statement, and you then drive `AdbcConnectionCommit` / `AdbcConnectionRollback`. Setting it back to `true` while a transaction is open commits that transaction first. The value must be exactly `"true"` or `"false"`; anything else results in `ADBC_STATUS_INVALID_ARGUMENT` error. |
| `firebolt.token` | inherited from the database | Per-connection bearer token. Two connections sharing one `AdbcDatabase` keep independent identities; setting it here never mutates the database default or the other connection. It is deliberately kept out of the session parameters below, because those are URL-encoded into every request line and would put the token in proxy and server access logs. May be set between `New` and `Init` — `Init` will not overwrite it. |
| *any other key* | — | Stored as a **session parameter** and appended to the query URL of every subsequent request as `<key>=<value>` (URL-encoded). This is how you pass Firebolt query settings through. |

Since unknown connection keys become session parameters, a typo here does not
raise — it is sent to the server, which may reject it or ignore it. This differs
on purpose from database options, where an unrecognised `firebolt.*` key is
an error.

### Server-driven session state

The server can change the connection's session parameters through response
headers, which the driver applies **only when the response was a success** — a
4xx/5xx body can come from a proxy or an attacker, so its session hints are not
trusted (`src/HttpClient.cpp`, `applySessionUpdatesIfSuccess`).

| Response header | Effect |
|-----------------|--------|
| `Firebolt-Update-Parameters` | Merge the listed `k=v` pairs into the session parameters. |
| `Firebolt-Remove-Parameters` | Drop the listed keys. |
| `Firebolt-Reset-Session` | Clear all session parameters. |

## Statement options

Set on `AdbcStatement`.

| Key | Default | Meaning |
|-----|---------|---------|
| `adbc.statement.bind_by_name` | `false` | `true` additionally carries each parameter under its bound column's name, for statements reading it through `param('name')`. Positional `$N` names are always sent regardless, so this only ever adds a naming. Must be exactly `"true"` or `"false"`; anything else is `ADBC_STATUS_INVALID_ARGUMENT`. See [Query parameters](#query-parameters). |
| `adbc.ingest.target_table` | — | Target table. Setting it switches `ExecuteUpdate`/`ExecuteQuery` to the ingest path, which generates `INSERT INTO <target> (<cols>) SELECT * FROM read_arrow('upload://data.arrow')` and uploads the bound Arrow data as a multipart request. |
| `adbc.ingest.target_catalog` | — | Catalog qualifier for the target table. |
| `adbc.ingest.target_db_schema` | — | Schema qualifier for the target table. |
| `adbc.ingest.mode` | `adbc.ingest.mode.create` | One of `adbc.ingest.mode.append`, `.create`, `.replace`, `.create_append`. Any other value is `ADBC_STATUS_INVALID_ARGUMENT`. |
| `adbc.ingest.temporary` | — | Only `false` (or empty) is accepted, as a no-op — the Python dbapi layer sends it by default. `true` returns `ADBC_STATUS_NOT_IMPLEMENTED`: Firebolt has no session-temporary tables. |

Bound Arrow data serves both paths, told apart by whether an ingest target table is
set: with one it is a bulk-ingest payload, without one it is query parameters. So
`adbc.ingest.mode`, `adbc.ingest.target_catalog` or `adbc.ingest.target_db_schema`
without `adbc.ingest.target_table` is `ADBC_STATUS_INVALID_STATE` at execution time,
not a silent parameter binding. Option order does not matter.

## Query parameters

Placeholders are **positional and written `$1`, `$2`, …** — Firebolt's own syntax.
`?`, `%s` and `:name` are not placeholders, and `$foo` lexes as an identifier.

The driver does not interpolate values into your SQL. They travel in the
`query_parameters` setting, a JSON array beside the statement, and the server
substitutes each into the *parsed* statement as a literal node — so a value
containing quotes or SQL keywords is data, not syntax.

```
POST /?database=db&output_format=ArrowStream&query_parameters=%5B%7B%22name%22%3A%22%241%22...
body: SELECT $1 + 1
```

Each bound row is one parameter set and one execution, for `ExecuteQuery` as much as
for `ExecuteUpdate` (what `executemany` calls), stopping at the first failure. A
result-returning execution hands back the last row's result set, there being one
stream to return. `rows_affected` stays `-1`; the server does not report it here.

Setting `query_parameters` as a *connection* option still works, but a bound
parameter set overrides it for that request rather than replacing it.

### Parameter types

A parameter's SQL type comes from the JSON type of its value — the format carries no
type field of its own.

| Bound Arrow type | Sent as | Parameter type |
|---|---|---|
| any `NULL` | `null` | untyped `NULL` (`pg_typeof` reports `unknown`) |
| `bool` | `true` / `false` | `BOOLEAN` |
| `int8`…`int64`, `uint8`…`uint32` | JSON integer | `BIGINT` |
| `uint64` | JSON integer | `BIGINT`; above `int64` max, `ADBC_STATUS_INVALID_ARGUMENT` |
| `float`, `double` | JSON number, always with a fraction | `DOUBLE`; `NaN`/infinity is `ADBC_STATUS_INVALID_ARGUMENT` |
| `string`, `large_string`, `string_view` | JSON string | `TEXT` |
| `date32`, `date64` | `"YYYY-MM-DD"` | `TEXT`, coerced |
| `timestamp` | `"YYYY-MM-DD HH:MM:SS[.ffffff]"`, `+00` when zoned | `TEXT`, coerced |
| `time32`, `time64` | `"HH:MM:SS[.ffffff]"` | `TEXT`, coerced |
| `decimal32/64/128/256` | exact digit string, never a JSON number | `TEXT`, coerced |
| `binary`, `list`, `struct`, `map`, `dictionary`, `interval`, `duration` | — | `ADBC_STATUS_NOT_IMPLEMENTED`, naming the type |

Whole floats keep a `.0` so they stay `DOUBLE`, and doubles print at the shortest
precision that round-trips. A decimal goes as a string because a JSON number is read
back through `std::stod`, which would round it.

Where a `TEXT` parameter meets a context that will not coerce it, cast explicitly:
`$1::DATE`, `$1::DECIMAL(38, 9)`.

### Named parameters

With `adbc.statement.bind_by_name=true` each parameter is *also* carried under its
bound column's name, which SQL reads through Firebolt's `param()` function.
`param()` returns `TEXT` whatever was sent, so cast for anything else:

```sql
SELECT param('who'), param('cutoff')::INT
```

The Python dbapi layer sets this option for you when you pass a dict.

Both namings are always sent. An unreferenced parameter is ignored by the server, so
the positional name costs one unused entry and keeps `$N` working when the option is
stale — which it can be, since a driver manager sends it only when its own idea of it
changes and the Python layer never revises it once parameters arrive as Arrow data.
An alias duplicating another parameter's name is dropped (the server rejects
duplicates); an unnamed column has no alias.

Treat the positional names as that safety net, not as a second way to address a named
parameter. `$N` is the bound column's ordinal, and for a dict that is the key order of
the dict — `{"who": …, "cutoff": …}` makes `$1` the value of `who`, and swapping the
two keys swaps what `$1` means, with no error either way. Use one style per
statement.

### Parameter metadata

`AdbcStatementPrepare` issues no request: Firebolt has no server-side prepare, so a
parameterised statement is sent whole every time. `AdbcStatementGetParameterSchema`
is where the round-trip happens — it re-sends the statement with
`execution_mode=describe_parameters`, which validates it and reports its placeholder
types without executing it.

The answer is **not** cached: it depends on the objects the statement names, so DDL
changes it while the text stays the same, and a driver manager re-issues
`SetSqlQuery` only on a text change. The cost is one request per call, and the call
is reached only from `cursor.adbc_prepare()`, never from `execute()`.

That request omits the `transaction_id` and `transaction_sequence_id` session
parameters — as Firebolt's own PostgreSQL-wire `Describe` does — so type inference
never spends a transaction step on a statement the caller has not run. It therefore
cannot see objects created in an open transaction: with autocommit off,
`adbc_prepare` against a table the same cursor just created reports that the table
does not exist. Commit first.

The returned schema is a struct with one field per parameter, named `$1`, `$2`, … and
ordered by ordinal position (by name, `$10` would precede `$2`). An unrecognised type
becomes Arrow `null` (NA), as ADBC prescribes for an undetermined parameter type.
Every field is nullable whatever the server reports: a nullability marker there
describes the column the placeholder was compared against, and `NULL` binds to any
parameter.

All identifiers — table, catalog, schema, and every column and struct field name
— are double-quoted with embedded `"` doubled before being interpolated into
generated SQL (`quoteIdentifier` in `src/IngestSqlBuilder.cpp`).

### What each ingest mode emits

| Mode | Statements, in order |
|------|----------------------|
| `append` | `INSERT INTO …` |
| `create` | `CREATE TABLE …`, `INSERT INTO …` |
| `create_append` | `CREATE TABLE IF NOT EXISTS …`, `INSERT INTO …` |
| `replace` | `DROP TABLE IF EXISTS …`, `CREATE TABLE …`, `INSERT INTO …` |

Each statement is a separate HTTP request — Firebolt rejects several statements
in one body. The create-style modes need the bound schema to synthesise the DDL,
so binding data with no schema fails with `ADBC_STATUS_INVALID_STATE`.

If an ingest target is set but no data was bound, the driver fails with
`ADBC_STATUS_INVALID_STATE` **before** issuing any statement. Without that gate,
`replace` would drop the existing table and then have nothing to insert.

### Arrow → Firebolt types used for generated DDL

From `arrowTypeToFireboltSqlType` in `src/IngestSqlBuilder.cpp`. This is the
mapping used to generate DDL for create-style ingest; reading results is
unaffected.

**A mapping existing here does not mean the ingest succeeds.** The DDL is only the
first of three steps — the Arrow data still has to be serialised to IPC by
nanoarrow and then read back by the server, and some types fail at one of those.
The verified end-to-end results are the
[Arrow to Database table in the README](README.md#arrow-to-database); use that
table to decide what to send. This one explains what the driver *emits*.

| Arrow type | Emitted Firebolt type | Ingest works |
|------------|----------------------|--------------|
| `bool` | `BOOLEAN` | yes |
| `int8`, `int16`, `int32`, `uint8`, `uint16` | `INT` | yes |
| `int64`, `uint32` | `BIGINT` | yes |
| `uint64` | `BIGINT` | only up to `int64` max — `BIGINT` is signed |
| `float32` | `REAL` | yes |
| `float64` | `DOUBLE PRECISION` | yes |
| `string`, `large_string` | `TEXT` | yes |
| `string_view` | `TEXT` | **no** — nanoarrow's IPC writer cannot encode view layouts |
| `binary`, `large_binary`, `fixed_size_binary` | `BYTEA` | yes |
| `binary_view` | `BYTEA` | **no** — same IPC limitation |
| `date32`, `date64` | `DATE` | yes |
| `timestamp` without timezone | `TIMESTAMPNTZ` | yes, all units |
| `timestamp` with timezone | `TIMESTAMPTZ` | yes; normalised to UTC, offset not retained |
| `decimal128` | `DECIMAL(p, s)` | yes, precision ≤ 38 |
| `decimal32`, `decimal64`, `decimal256` | `DECIMAL(p, s)` | **no** — the server cannot read these Arrow layouts |
| `list`, `large_list` | `ARRAY(<inner>)`, recursive | yes |
| `fixed_size_list` | `ARRAY(<inner>)` | **no** — the server rejects the schema |
| `struct` | `STRUCT("field" TYPE, …)`, recursive | yes |

Anything else — `map`, `null`, `duration`, intervals, unions, `time32`/`time64` —
has no mapping, and create-style ingest fails with
`ADBC_STATUS_NOT_IMPLEMENTED` rather than sending DDL the server would reject.
`dictionary` has no mapping either and additionally cannot be IPC-encoded.

Two nullability rules, both forced by Firebolt:

- A non-nullable **column** is emitted as `NOT NULL`.
- A non-nullable **struct field** is widened to nullable, because Firebolt
  rejects `STRUCT(… NOT NULL)` with "STRUCT fields have to be nullable".
- A zero-field struct has no mapping at all: there is no `STRUCT()` in Firebolt,
  so the ingest fails instead of producing a syntax error at the server.

## Unknown options

| Level | Unknown key behaviour |
|-------|----------------------|
| Database | `firebolt.*` → `ADBC_STATUS_NOT_FOUND`. Any other key is accepted and ignored, because the driver manager sets some itself and callers pass parameters this driver does not consume yet — except `username` and `password`, which are [FB2 only](#fb2-only-options) credentials. |
| Connection | Accepted, and forwarded to the server as a session parameter. |
| Statement | Accepted and ignored. |

---

## Where this is verified

The behaviour above is pinned against the engine image the integration suite runs
(`tests/integration/runner.py::DEFAULT_ENGINE_IMAGE`), which accepts
`query_parameters` and `execution_mode` as ordinary URL query parameters. Neither is
in the engine's curated public-settings list; a managed SaaS gateway that filters
non-public settings would need another channel for binding.

---

## Names that are going to change

The driver's option names predate
[`sdk-authentication.md`](https://github.com/firebolt-db/packdb/blob/main/specs/sdk-authentication.md),
which standardises one parameter set across every Firebolt SDK — canonical
names, types, and defaults live in
[`specs/schemas/connection-parameters.v1.json`](https://github.com/firebolt-db/packdb/blob/main/specs/schemas/connection-parameters.v1.json).
This driver has not migrated yet. The table below is so you can tell which of
today's names are stable and which are already superseded.

| Today | Canonical | Note |
|-------|-----------|------|
| `uri` | `host` | A `firebolt://` URI already carries the host, database and `ssl_mode`; the `http(s)://` form encodes the transport in the scheme instead. |
| `firebolt.database` | `database` | Rename only. |
| `firebolt.timeout_sec` | `query_timeout` | Rename only. |
| `firebolt.token` | *(none)* | The spec has no connection field for a raw JWT: it comes from the `FIREBOLT_TOKEN` environment variable. This key will be removed. |
| — | `username` / `password` | OAuth `client_id` / `client_secret` for the `client_credentials` grant. Not implemented yet. |
| — | `engine` | Engine selector, sent per request. Not implemented yet. |
| — | `authorization_server` | Which discovered authorization server to use. Not implemented yet. |
| `firebolt.ssl_certificate_path` | `ssl_certificate_path` | Rename only. |
| — | `ssl_mode` | Accepted inside a `firebolt://` URI (`verify-full`, the default, or `disable`), not yet as an option of its own; with an `http(s)://` `uri` the scheme decides. Verification cannot be relaxed. |
| — | `use_token_cache`, `connection_timeout`, `max_retries`, `user_agent` | Not implemented yet. |

When the migration lands, the current names are replaced rather than aliased.
Nothing outside this repository consumes them yet, and carrying two spellings
forever is worse than one rename before the first public release.

---

## FB2 only options

These apply only to engines in a **Firebolt 2.0 SaaS** account, and are documented
in full in [docs/fb2-saas.md](docs/fb2-saas.md). Setting `firebolt.account`
selects that mode; `uri` must then be left out.

| Key | Meaning |
|-----|---------|
| `username` / `firebolt.client_id` | Service account ID |
| `password` / `firebolt.client_secret` | Service account secret |
| `firebolt.account` | Account name (required) |
| `firebolt.engine` | Engine name (required) |
| `firebolt.cache_connection` | `true` (default) or `false`: the in-process token and engine-URL cache |

`firebolt.database`, `firebolt.token`, `firebolt.timeout_sec` and
`firebolt.ssl_certificate_path` keep their meaning above.
