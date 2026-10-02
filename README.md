# Firebolt ADBC driver

A standalone C++ shared library implementing the
[ADBC 1.1.0](https://arrow.apache.org/adbc/) C API against Firebolt's HTTP query
interface. Results arrive as Arrow record batches with no row-by-row conversion,
so a query lands straight in pyarrow, pandas, or polars.

It is a client-side library only: a driver manager
([Python](https://pypi.org/project/adbc-driver-manager/),
[R](https://cran.r-project.org/package=adbcdrivermanager), Go, …) loads
`libfirebolt_adbc.so` at runtime. Nothing needs to be installed on the server.

## Supported today

Read this before you build anything on it.

| | |
|---|---|
| **Transport** | `http://` and `https://`. TLS verifies the peer against the system CA bundle, found at run time; `adbc.firebolt.ssl_certificate_path` names another. There is no way to switch verification off. |
| **Authentication** | Engines with authentication **disabled**, plus an optional bearer token you obtained elsewhere. Firebolt's discovery-based OAuth flow is **not** implemented. See [docs/authentication.md](docs/authentication.md). |
| **Platforms** | Linux x86_64 and aarch64, glibc 2.34 or newer. No macOS or Windows build. |
| **ADBC** | The full ADBC 1.0.0 function set. Reports itself as 1.1.0, but the 1.1.0-only entry points are not implemented — see [Feature & Type Support](#feature--type-support). |

In practice that means this driver is ready for local development, CI, and
trusted-network deployments where the engine does not require authentication. It
cannot yet obtain a token itself, so an engine that requires authentication needs a
token you acquired elsewhere.

## Quickstart

Takes about two minutes and needs Docker plus Python 3.9+.

**1. Install the driver manager**

```bash
pip install adbc-driver-manager pyarrow
```

**2. Download the driver**

Grab `libfirebolt_adbc-x86_64.so` (or `-aarch64`) from the
[latest release](https://github.com/firebolt-db/firebolt-adbc/releases/latest):

```bash
curl -sSLO https://github.com/firebolt-db/firebolt-adbc/releases/latest/download/libfirebolt_adbc-x86_64.so
curl -sSLO https://github.com/firebolt-db/firebolt-adbc/releases/latest/download/libfirebolt_adbc-x86_64.so.sha256
sha256sum -c libfirebolt_adbc-x86_64.so.sha256
```

**3. Start a Firebolt engine**

```bash
docker run -d --name firebolt -p 3473:3473 ghcr.io/firebolt-db/engine:latest
```

With no configuration the engine starts as a single node with authentication
disabled and a database called `firebolt` — which is why the quickstart needs no
credentials. Give it a few seconds, then check it is up:

```bash
curl -fsS http://localhost:3473/ping && echo ok
```

**4. Query it**

```python
import adbc_driver_manager.dbapi as dbapi

with dbapi.connect(
    driver="./libfirebolt_adbc-x86_64.so",
    db_kwargs={"uri": "http://localhost:3473"},
) as conn:
    with conn.cursor() as cur:
        cur.execute("SELECT 1 AS n, 'hello' AS greeting")
        print(cur.fetch_arrow_table())
```

```
pyarrow.Table
n: int32 not null
greeting: string not null
----
n: [[1]]
greeting: [["hello"]]
```

That is the whole setup. Runnable versions of everything below are in
[`examples/python/`](examples/python).

### Naming the driver instead of its path

If you would rather write `driver="firebolt"` than an absolute path, install the
[`firebolt.toml`](firebolt.toml) manifest into a directory the driver manager
searches (`~/.config/adbc/drivers`, `/etc/adbc/drivers`, or anything on
`ADBC_DRIVER_PATH`) and point its `Driver.shared` entries at your `.so`. Needs
`adbc-driver-manager >= 1.5`.

## Common tasks

### Into pandas or polars

```python
cur.execute("SELECT * FROM my_table")

table = cur.fetch_arrow_table()   # pyarrow.Table
df = cur.fetch_df()               # pandas.DataFrame
```

For a result larger than memory, stream it in batches instead:

```python
cur.execute("SELECT * FROM big_table")
for batch in cur.fetch_record_batch():
    process(batch)
```

### Bulk-loading Arrow data

`adbc_ingest` uploads Arrow data directly — no `INSERT` statement to build, no
per-row round trips. Nested `ARRAY` and `STRUCT` columns are supported, including
the `CREATE TABLE` the driver generates for you:

```python
import pyarrow as pa

table = pa.table({
    "id":   pa.array([1, 2], pa.int32()),
    "tags": pa.array([["a", "b"], ["c"]], pa.list_(pa.string())),
    "meta": pa.array([{"k": 1}, {"k": 2}], pa.struct([("k", pa.int32())])),
})

with dbapi.connect(driver=DRIVER, db_kwargs={"uri": URI}, autocommit=True) as conn:
    with conn.cursor() as cur:
        cur.adbc_ingest("events", table, mode="replace")
```

`mode` is `append`, `create`, `create_append`, or `replace`. Which Arrow types you
can ingest, and what each becomes, is in
[Arrow to Database](#arrow-to-database) below.

### Transactions

`dbapi.connect()` defaults to `autocommit=False` for PEP 249 compliance, so you
are in a transaction whether or not you asked for one and the driver issues
`BEGIN` before your first statement:

```python
with dbapi.connect(driver=DRIVER, db_kwargs={"uri": URI}, autocommit=False) as conn:
    cur = conn.cursor()
    cur.execute("INSERT INTO events VALUES (1, 'a')")
    conn.commit()   # or conn.rollback()
```

Pass `autocommit=True` for each statement to stand on its own.

### Query parameters

Placeholders are positional: `$1`, `$2`, … — Firebolt's own syntax, not `?` or `%s`.
Values are never spliced into your SQL; they travel beside it and the server
substitutes them into the parsed statement, so a value containing quotes or SQL
keywords is data.

```python
cur.execute("SELECT $1 + 1", (41,))                     # → 42
cur.execute("SELECT id FROM events WHERE label = $1", ("a'; DROP TABLE events; --",))
cur.executemany("INSERT INTO events VALUES ($1, $2)", [(1, "a"), (2, "b")])
```

Each parameter keeps its type: integers as `BIGINT`, floats as `DOUBLE`, booleans as
`BOOLEAN`, `None` as an untyped `NULL`. Dates, timestamps and decimals travel as text
and coerce at the use site — cast (`$1::DATE`) where the context does not imply one.
`bytes` and nested values cannot be bound <sup>[4](#fn4)</sup>.

Named parameters are read through Firebolt's `param()` function, and are always
`TEXT`. Pass a dict to name them:

```python
cur.execute("SELECT param('who')", {"who": "ann"})
cur.execute("SELECT param('cutoff')::INT + 1", {"cutoff": 41})
```

Use one style per statement. A dict also binds `$1`, `$2`, … — to its keys in
insertion order — so a statement mixing the two changes meaning when the dict literal
is reordered <sup>[17](#fn17)</sup>.

`cur.adbc_prepare(sql)` returns the parameter schema the server infers, without
running the statement:

```python
cur.adbc_prepare("SELECT label FROM events WHERE id = $1")   # → schema: $1: int32
```

### Metadata

```python
conn.adbc_get_info()                              # driver and vendor identity
conn.adbc_get_table_types()                       # e.g. BASE TABLE, VIEW
conn.adbc_get_table_schema("events")              # pyarrow.Schema
conn.adbc_get_objects(depth="columns")            # catalogs → schemas → tables → columns
```

## Feature & Type Support

### Features

| Feature | Firebolt |
|---------|----------|
| Bulk Ingestion: Create | ✅ |
| Bulk Ingestion: Append | ✅ |
| Bulk Ingestion: Create/Append | ✅ |
| Bulk Ingestion: Replace | ✅ |
| Bulk Ingestion: Temporary Table | ❌ <sup>[1](#fn1)</sup> |
| Bulk Ingestion: Target Catalog | ✅ <sup>[2](#fn2)</sup> |
| Bulk Ingestion: Target Schema | ✅ |
| Non-nullable fields are marked NOT NULL | ✅ <sup>[3](#fn3)</sup> |
| Catalog (GetObjects): depth=catalogs | ✅ |
| Catalog (GetObjects): depth=db_schemas | ✅ |
| Catalog (GetObjects): depth=tables | ✅ |
| Catalog (GetObjects): depth=columns (all) | ✅ |
| Get Parameter Schema | ✅ <sup>[5](#fn5)</sup> |
| Get Table Schema | ✅ |
| Prepared Statements | ✅ <sup>[5](#fn5)</sup> |
| Transactions | ✅ |

### Beyond the standard table

Driver-level capabilities the shared table does not cover.

| | Firebolt |
|---|----------|
| Result streaming as Arrow record batches | ✅ |
| Session parameter passthrough, server-driven session updates | ✅ |
| Per-connection bearer token | ✅ |
| Query parameter binding (`execute(sql, params)`) | ✅ `$1`, `$2`, … <sup>[4](#fn4)</sup> |
| Named parameter binding (`execute(sql, {...})`) | ✅ via `param('name')` <sup>[17](#fn17)</sup> |
| `rowcount` on DML | ❌ always `-1` <sup>[6](#fn6)</sup> |
| TLS / `https://` endpoints | ✅ verified against the system CA bundle |
| Discovery-based authentication | ❌ see [docs/authentication.md](docs/authentication.md) |
| Partitioned execution, Substrait plans | ❌ |
| ADBC 1.1.0-only entry points <sup>[7](#fn7)</sup> | ❌ |

### Types

#### Database to Arrow

What each Firebolt column type becomes when you read it. Firebolt has no
`SMALLINT` and no `TIME` type; `INT`/`INTEGER`, `VARCHAR`/`TEXT`, and
`FLOAT`/`DOUBLE`/`DOUBLE PRECISION` are aliases that collapse as shown.

| Database Type | Firebolt |
|---|---|
| `BOOLEAN` | `bool` |
| `INTEGER` (`INT`) | `int32` |
| `BIGINT` | `int64` |
| `REAL` | `float` |
| `DOUBLE PRECISION` (`FLOAT`, `DOUBLE`) | `double` |
| `TEXT` (`VARCHAR`) | `string` |
| `BYTEA` | `binary` |
| `DATE` | `date32[day]` |
| `TIMESTAMP` (`TIMESTAMPNTZ`) | `timestamp[us]` |
| `TIMESTAMPTZ` | `timestamp[us, tz=UTC]` |
| `NUMERIC(p, s)` (`DECIMAL(p, s)`) | `decimal128(p, s)` |
| `GEOGRAPHY` | `binary` <sup>[8](#fn8)</sup> |
| `ARRAY(T)` | `list<item: T>` |
| `STRUCT(…)` | `struct<…>` |

Nesting is preserved to any depth: `ARRAY(ARRAY(INT))` reads as
`list<item: list<item: int32>>`, and `ARRAY(STRUCT("k" INT))` as
`list<item: struct<k: int32>>`.

#### Arrow to Database

Two paths, with different limits. **Ingest** generates `CREATE TABLE` DDL from the
bound Arrow schema and uploads the data as Arrow IPC. **Bind** sends the value as a
query parameter, a format carrying only `NULL`, booleans, numbers and strings — so
anything else is rendered as text and cast, or cannot be sent.

| Arrow Type | Firebolt Type | Ingest | Bind |
|---|---|---|---|
| `bool` | `BOOLEAN` | ✅ | ✅ |
| `int8`, `int16`, `int32`, `uint8`, `uint16` | `INTEGER` | ✅ | ✅ as `BIGINT` |
| `int64`, `uint32` | `BIGINT` | ✅ | ✅ |
| `uint64` | `BIGINT` | ⚠️ <sup>[9](#fn9)</sup> | ⚠️ <sup>[9](#fn9)</sup> |
| `float` | `REAL` | ✅ | ✅ as `DOUBLE` |
| `double` | `DOUBLE PRECISION` | ✅ | ✅ <sup>[18](#fn18)</sup> |
| `string`, `large_string` | `TEXT` | ✅ | ✅ |
| `string_view` | `TEXT` | ❌ <sup>[10](#fn10)</sup> | ✅ |
| `binary`, `large_binary`, `fixed_size_binary` | `BYTEA` | ✅ | ❌ <sup>[4](#fn4)</sup> |
| `binary_view` | `BYTEA` | ❌ <sup>[10](#fn10)</sup> | ❌ <sup>[4](#fn4)</sup> |
| `date32[day]`, `date64[ms]` | `DATE` | ✅ | ✅ as text <sup>[19](#fn19)</sup> |
| `timestamp[s\|ms\|us\|ns]` | `TIMESTAMP` | ✅ | ✅ as text <sup>[19](#fn19)</sup> |
| `timestamp` (with time zone) | `TIMESTAMPTZ` | ✅ <sup>[11](#fn11)</sup> | ✅ as text <sup>[19](#fn19)</sup> |
| `time32`, `time64` | (no mapping) | ❌ <sup>[16](#fn16)</sup> | ✅ as text <sup>[19](#fn19)</sup> |
| `decimal128(p, s)` | `NUMERIC(p, s)` | ✅ <sup>[12](#fn12)</sup> | ✅ as text <sup>[19](#fn19)</sup> |
| `decimal32`, `decimal64`, `decimal256` | `NUMERIC(p, s)` | ❌ <sup>[13](#fn13)</sup> | ✅ as text <sup>[19](#fn19)</sup> |
| `list<T>`, `large_list<T>` | `ARRAY(T)` | ✅ | ❌ <sup>[4](#fn4)</sup> |
| `fixed_size_list<T>` | `ARRAY(T)` | ❌ <sup>[14](#fn14)</sup> | ❌ <sup>[4](#fn4)</sup> |
| `struct<…>` | `STRUCT(…)` | ✅ <sup>[3](#fn3)</sup> | ❌ <sup>[4](#fn4)</sup> |
| `dictionary` | (no mapping) | ❌ <sup>[15](#fn15)</sup> | ❌ <sup>[4](#fn4)</sup> |
| `map` | (no mapping) | ❌ <sup>[16](#fn16)</sup> | ❌ <sup>[4](#fn4)</sup> |
| `null`, `duration`, `interval` | (no mapping) | ❌ <sup>[16](#fn16)</sup> | ❌ <sup>[4](#fn4)</sup> |

A `NULL` in any column binds as an untyped SQL `NULL` whatever the column's Arrow
type, including the types marked ❌ above.

`ARRAY` and `STRUCT` compose to any depth, so `list<struct<…>>` and
`struct<…, list<…>>` both generate correct DDL.

### Footnotes

1. <a id="fn1"></a>Firebolt has no session-temporary tables.
   `adbc.ingest.temporary=true` returns `ADBC_STATUS_NOT_IMPLEMENTED`; `false` is
   accepted as a no-op, which is what the Python dbapi layer sends by default.
2. <a id="fn2"></a>Honoured as the leading part of a qualified name. Set a schema
   alongside it: a catalog on its own renders a two-part name that Firebolt reads
   as `schema.table`.
3. <a id="fn3"></a>A non-nullable Arrow *column* becomes `NOT NULL`. A
   non-nullable *struct field* is widened to nullable, because Firebolt rejects
   `STRUCT(… NOT NULL)` with "STRUCT fields have to be nullable". A zero-field
   struct has no mapping — there is no `STRUCT()` in Firebolt — so ingest fails
   rather than emitting DDL the server would reject.
4. <a id="fn4"></a>Placeholders are `$1`, `$2`, … — see
   [Query parameters](#query-parameters). A parameter travels as JSON, a format
   carrying only `NULL`, booleans, numbers and strings, so binding `binary` or a
   nested type returns `ADBC_STATUS_NOT_IMPLEMENTED` naming the type. Ingest them
   instead, or encode them yourself.
5. <a id="fn5"></a>There is no server-side prepare — a parameterised statement is
   sent whole every time — so `Prepare` issues no request, and
   `cursor.execute(sql, params)` costs one HTTP request like any other query.
   `GetParameterSchema` does the work: the server validates the statement and reports
   its placeholder types without executing it. Each `adbc_prepare()` costs one
   request, uncached, since DDL can change the types under an unchanged statement.
   With autocommit off it cannot see objects created in the open transaction; see
   [OPTIONS.md](OPTIONS.md#parameter-metadata).
6. <a id="fn6"></a>The server does not report affected rows over this interface.
   Use `SELECT count(*)` when you need a number.
7. <a id="fn7"></a>`GetOption*`, `SetOptionInt`/`Double`/`Bytes`, `Cancel`,
   `ExecuteSchema`, `GetStatistics`, `ErrorGetDetail`. The driver implements the
   ADBC 1.0.0 function set, although `GetInfo` reports ADBC 1.1.0.
8. <a id="fn8"></a>Raw WKB bytes, with no `geoarrow` extension metadata — unlike
   some other drivers, which surface `extension<geoarrow.wkt>`.
9. <a id="fn9"></a>`BIGINT` is signed, so a value above `int64` max
   (9223372036854775807) is rejected — on ingest by the server with "Convert
   overflow", on bind by the driver with `ADBC_STATUS_INVALID_ARGUMENT`. Values at
   or below it round-trip exactly.
10. <a id="fn10"></a>The driver maps the view types to `TEXT`/`BYTEA`, but nanoarrow's
    IPC writer cannot encode Arrow's view layouts, so the *ingest* upload fails with
    `ADBC_STATUS_INTERNAL` before reaching the server; cast to `string`/`binary`
    first. Binding is unaffected — a parameter never travels as Arrow IPC.
11. <a id="fn11"></a>The instant is preserved and normalised to UTC; the original
    offset is not retained. `12:00+02:00` reads back as `10:00Z`.
12. <a id="fn12"></a>Precision must be 38 or less, Firebolt's maximum. Above that
    the server rejects it with "Decimal precision overflow".
13. <a id="fn13"></a>The server cannot read these Arrow decimal layouts, and
    `decimal256` fails even within precision 38. Cast to `decimal128`.
14. <a id="fn14"></a>The generated DDL is correct, but the server rejects the
    fixed-size-list schema in the uploaded file. Cast to `list`.
15. <a id="fn15"></a>nanoarrow's IPC writer refuses dictionary encoding. Decode to
    the value type before ingesting.
16. <a id="fn16"></a>No Firebolt equivalent, so ingest fails with
    `ADBC_STATUS_NOT_IMPLEMENTED` rather than uploading DDL the server would
    reject.
17. <a id="fn17"></a>Firebolt has no named *placeholder* (`$foo` is an identifier), so
    a named parameter is read through `param('name')`, which always yields `TEXT`.
    Cast it (`param('n')::INT`) for anything else. The driver sends the positional
    `$N` names alongside the aliases so a stale `adbc.statement.bind_by_name` cannot
    unbind a placeholder — a safety net, not a second addressing scheme: `$N` follows
    the dict's key order, which the naming API otherwise never asks you to think
    about.
18. <a id="fn18"></a>Bound as a JSON number with a fractional part, so `3.0` stays a
    `DOUBLE` rather than arriving as a `BIGINT`. `NaN` and infinities have no JSON
    or SQL literal and are rejected with `ADBC_STATUS_INVALID_ARGUMENT`.
19. <a id="fn19"></a>Sent as a string in canonical text form, the parameter format
    having no date, time or decimal type. Firebolt coerces it in most contexts;
    elsewhere cast explicitly (`$1::DATE`, `$1::DECIMAL(38, 9)`). A decimal is never
    a JSON number, which would round it through a `double`.

## Options

The four database options, in full:

| Key | Required | Meaning |
|-----|----------|---------|
| `uri` | **yes** | Engine HTTP endpoint, e.g. `http://localhost:3473`. |
| `adbc.firebolt.database` | no | Database name; sent as `database=` on every request. |
| `adbc.firebolt.token` | no | Bearer token. Omit for an engine with authentication disabled. |
| `adbc.firebolt.timeout_sec` | no | Request timeout in whole seconds; `0` (the default) disables it. |

Connection and statement options, ingest modes, the session-parameter protocol,
and the type mapping are all in **[OPTIONS.md](OPTIONS.md)**.

Note that these names are provisional: Firebolt is standardising one parameter
set across all its SDKs, and this driver has not migrated yet. The mapping from
today's names to the canonical ones is in
[OPTIONS.md](OPTIONS.md#names-that-are-going-to-change).

## Troubleshooting

| Symptom | Cause and fix |
|---------|---------------|
| `Database 'uri' option is required` | No `uri` in `db_kwargs`. |
| `Database 'uri' must start with http:// or https://` | You passed a bare host (`localhost:3473`) or a `firebolt://` URI. This driver takes the engine's HTTP endpoint. |
| `... is https:// but this driver was built without TLS support` | A self-built driver configured with `-DWITH_SSL=OFF`. Released builds have TLS; rebuild with the default `WITH_SSL=ON`. |
| `No CA certificate bundle found for https://` | The host has no system CA certificates (common in minimal containers). Install them (`ca-certificates` on Debian/Ubuntu/RHEL), or set `adbc.firebolt.ssl_certificate_path` to a PEM bundle. |
| `IO: curl error: SSL peer certificate or SSH remote key was not OK` | The server's certificate does not chain to a trusted CA, or its name does not match the `uri` host. For a private CA, point `adbc.firebolt.ssl_certificate_path` at its PEM file. |
| `IO: curl error: Couldn't connect to server` | Nothing is listening. Check the container is up and the port matches: `curl -fsS http://localhost:3473/ping`. |
| `Cluster not yet healthy` | The engine answers `/ping` before it can serve queries. Retry `SELECT 1` for a few seconds. |
| `UNAUTHORIZED: HTTP 401` / `403` | The engine wants authentication. Supply `adbc.firebolt.token`; see [docs/authentication.md](docs/authentication.md). |
| `Query referenced positional parameter $1, but it was not set` | The statement has more `$N` placeholders than you passed values for. Note `$1` is 1-based. |
| `NOT_IMPLEMENTED: Cannot bind a parameter of Arrow type binary` | `bytes`, lists and structs cannot be query parameters <sup>[4](#fn4)</sup>. Ingest them, or encode them to text yourself. |
| `INVALID_ARGUMENT: Parameter value … exceeds the BIGINT range` | A `uint64` above `int64` max; Firebolt parameters are signed. |
| `INVALID_ARGUMENT: Parameter value is not finite` | `NaN` or an infinity was bound. Neither has a JSON or SQL literal. |
| `INVALID_ARGUMENT: Parameter value … is not a valid time of day` | A negative `time32`/`time64` value. |
| `execute()` with several parameter sets returns only one result set | Expected: the statement runs once per bound row, and the returned result is the last execution's. Use `executemany()` when you do not want a result. |
| A parameter compares as text against a typed column | Add an explicit cast: `$1::DATE`, `param('n')::INT` <sup>[19](#fn19)</sup>. |
| `NOT_FOUND: Unknown Firebolt database option '…'` | A misspelled `adbc.firebolt.*` key. Compare against [OPTIONS.md](OPTIONS.md). |
| `NOT_IMPLEMENTED: Temporary ingest tables are not supported` | `adbc_ingest(..., temporary=True)`. Firebolt has no session-temporary tables. |
| `NOT_IMPLEMENTED: ingest column type cannot be mapped …` | The Arrow schema has a type with no Firebolt equivalent. Cast it before ingesting. |
| `current transaction is aborted, commands will be ignored …` | A statement failed inside an open transaction. Call `conn.rollback()`. `dbapi.connect()` disables autocommit by default, so you may be in a transaction you did not open. |
| `INVALID_ARGUMENT: Option 'adbc.connection.autocommit' must be exactly …` | Use the string `"true"` or `"false"`; `"0"` and `"FALSE"` are refused rather than guessed at. |
| `dlopen() failed: … cannot open shared object file` | The driver path is wrong, or the manifest name does not match the `driver=` value. |
| `cursor.rowcount` is `-1` | Expected; see [Feature & Type Support](#feature--type-support). Use `SELECT count(*)` if you need a count. |

## Build from source

Only needed to develop the driver — consumers should use a
[release](https://github.com/firebolt-db/firebolt-adbc/releases). Requires Docker
and git.

```bash
./scripts/build.sh              # → build/libfirebolt_adbc.so
./scripts/test-unit.sh          # C++ unit tests, no server needed
./scripts/test-integration.sh   # pytest against a throwaway 1-node engine
```

`scripts/build.sh` builds inside a pinned Ubuntu 22.04 + clang-18 image, so the
resulting `.so` runs on older glibc than the host. Details and the dependency
policy are in [CONTRIBUTING.md](CONTRIBUTING.md) and [CLAUDE.md](CLAUDE.md).

## Documentation

| | |
|---|---|
| [OPTIONS.md](OPTIONS.md) | Every option at all three ADBC levels |
| [docs/authentication.md](docs/authentication.md) | What works now, Firebolt's actual auth model, and the gaps |
| [examples/python/](examples/python) | Runnable scripts for each task above |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Building, testing, and submitting changes |
| [CHANGELOG.md](CHANGELOG.md) | Release history |

## License

Apache 2.0 — see [LICENSE](LICENSE).
