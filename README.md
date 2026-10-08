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

# Firebolt ADBC driver

Query [Firebolt](https://www.firebolt.io/) from Python, R, Go and any other
language with an [ADBC](https://arrow.apache.org/adbc/) driver manager. Results
arrive as Apache Arrow, so they land straight in pyarrow, pandas or polars with
no row-by-row conversion.

The driver is a single shared library loaded by your application; nothing is
installed on the server.

## Supported today

| | |
|---|---|
| **Connections** | `http://` and `https://` (always certificate-verified) |
| **Authentication** | **Firebolt SaaS:** fully supported, with a service account ([docs/fb2-saas.md](docs/fb2-saas.md)). **Firebolt Core:** authentication disabled, or a bearer token you already have ([docs/authentication.md](docs/authentication.md)). |
| **Platforms** | Linux x86_64/aarch64 (glibc 2.28+), macOS arm64, Windows x86_64 |
| **ADBC** | The ADBC 1.0.0 API (see [Feature & Type Support](#feature--type-support)) |

## Quickstart

You need Docker and Python 3.9+. It takes about two minutes.

**1. Install the driver manager and the driver** (the driver via [dbc](https://docs.columnar.tech/dbc)):

```bash
pip install adbc-driver-manager pyarrow
dbc install firebolt --pre
```

**2. Start a local Firebolt engine**

```bash
docker run -d --name firebolt -p 3473:3473 ghcr.io/firebolt-db/engine:5.0.0-pre.0.20260927210425.e91cd5bd17f8
curl -fsS http://localhost:3473/ping && echo ok   # wait until this prints "ok"
```

It starts with authentication disabled, so no credentials are needed.

**3. Run a query**

```python
import adbc_driver_manager.dbapi as dbapi

with dbapi.connect(
    driver="firebolt", db_kwargs={"uri": "http://localhost:3473"}
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

Runnable versions of everything below are in [`examples/python/`](examples/python).

## Common tasks

### Fetch results

```python
cur.execute("SELECT * FROM my_table")
table = cur.fetch_arrow_table()  # pyarrow.Table
df = cur.fetch_df()  # pandas.DataFrame

# Larger than memory? Stream it:
cur.execute("SELECT * FROM big_table")
for batch in cur.fetch_record_batch():
    process(batch)
```

### Load Arrow data into a table

`adbc_ingest` uploads a whole Arrow table in one request and can create the
target table for you, nested `ARRAY` and `STRUCT` columns included:

```python
import pyarrow as pa

table = pa.table(
    {
        "id": pa.array([1, 2], pa.int32()),
        "tags": pa.array([["a", "b"], ["c"]], pa.list_(pa.string())),
    }
)

with dbapi.connect(driver="firebolt", db_kwargs={"uri": URI}, autocommit=True) as conn:
    with conn.cursor() as cur:
        # mode: create, append, create_append or replace
        cur.adbc_ingest("events", table, mode="replace")
```

See [Arrow to Database](#arrow-to-database) for the supported types.

### Query parameters

Use Firebolt's positional placeholders `$1`, `$2`, … (not `?` or `%s`). Values
are sent separately from the SQL, never spliced into it:

```python
cur.execute("SELECT $1 + 1", (41,))  # → 42
cur.executemany("INSERT INTO events VALUES ($1, $2)", [(1, "a"), (2, "b")])
```

Numbers, booleans, strings and `None` keep their type. Dates, timestamps and
decimals are sent as text — add a cast (`$1::DATE`) if Firebolt cannot infer one.

Named parameters use Firebolt's `param()` function and are always `TEXT`:

```python
cur.execute("SELECT param('cutoff')::INT + 1", {"cutoff": 41})
```

More in [OPTIONS.md](OPTIONS.md#query-parameters).

### Transactions

`dbapi.connect()` turns autocommit off by default (as PEP 249 requires), so your
statements run in a transaction until you commit:

```python
cur.execute("INSERT INTO events VALUES (1, 'a')")
conn.commit()  # or conn.rollback()
```

Pass `autocommit=True` to `dbapi.connect()` to run each statement on its own.

### Explore metadata

```python
conn.adbc_get_table_schema("events")  # pyarrow.Schema
conn.adbc_get_objects(depth="columns")  # catalogs → schemas → tables → columns
conn.adbc_get_table_types()
conn.adbc_get_info()
```

## Options

Set these in `db_kwargs`:

| Key | Meaning |
|-----|---------|
| `uri` | **Required.** The engine endpoint, e.g. `http://localhost:3473`, or `firebolt://host[:port]/[database]` ([details](OPTIONS.md#firebolt-uris)) |
| `firebolt.database` | Database to use |
| `firebolt.token` | Bearer token; omit when authentication is disabled |
| `firebolt.timeout_sec` | Request timeout in seconds; `0` (default) means none |
| `firebolt.ssl_certificate_path` | PEM CA bundle for `https://`, if not the system one |

Connection and statement options are in **[OPTIONS.md](OPTIONS.md)**. These names
will change when the driver adopts Firebolt's common SDK parameter names
([mapping](OPTIONS.md#names-that-are-going-to-change)).

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
| Get Parameter Schema | ✅ <sup>[4](#fn4)</sup> |
| Get Table Schema | ✅ |
| Prepared Statements | ✅ <sup>[4](#fn4)</sup> |
| Transactions | ✅ |

| Also | Firebolt |
|---|----------|
| Streaming results as Arrow record batches | ✅ |
| Query parameters (`$1`, named via `param('name')`) | ✅ <sup>[5](#fn5)</sup> |
| Session parameters (`firebolt.session.*`) | ✅ |
| `rowcount` after DML | ❌ always `-1` <sup>[6](#fn6)</sup> |
| Firebolt Core sign-in with credentials | ❌ see [docs/authentication.md](docs/authentication.md) |
| Partitioned execution, Substrait | ❌ |
| ADBC 1.1.0-only functions | ❌ <sup>[7](#fn7)</sup> |

### Types

#### Database to Arrow

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

`ARRAY` and `STRUCT` nest to any depth.

#### Arrow to Database

**Ingest** creates and loads tables from Arrow data; **Bind** sends values as
query parameters.

| Arrow Type | Firebolt Type | Ingest | Bind |
|---|---|---|---|
| `bool` | `BOOLEAN` | ✅ | ✅ |
| `int8`, `int16`, `int32`, `uint8`, `uint16` | `INTEGER` | ✅ | ✅ as `BIGINT` |
| `int64`, `uint32` | `BIGINT` | ✅ | ✅ |
| `uint64` | `BIGINT` | ⚠️ <sup>[9](#fn9)</sup> | ⚠️ <sup>[9](#fn9)</sup> |
| `float` | `REAL` | ✅ | ✅ as `DOUBLE` |
| `double` | `DOUBLE PRECISION` | ✅ | ✅ <sup>[10](#fn10)</sup> |
| `string`, `large_string` | `TEXT` | ✅ | ✅ |
| `string_view` | `TEXT` | ❌ <sup>[11](#fn11)</sup> | ✅ |
| `binary`, `large_binary`, `fixed_size_binary` | `BYTEA` | ✅ | ❌ <sup>[5](#fn5)</sup> |
| `binary_view` | `BYTEA` | ❌ <sup>[11](#fn11)</sup> | ❌ <sup>[5](#fn5)</sup> |
| `date32[day]`, `date64[ms]` | `DATE` | ✅ | ✅ as text <sup>[12](#fn12)</sup> |
| `timestamp[s\|ms\|us\|ns]` | `TIMESTAMP` | ✅ | ✅ as text <sup>[12](#fn12)</sup> |
| `timestamp` (with time zone) | `TIMESTAMPTZ` | ✅ <sup>[13](#fn13)</sup> | ✅ as text <sup>[12](#fn12)</sup> |
| `time32`, `time64` | (no mapping) | ❌ | ✅ as text <sup>[12](#fn12)</sup> |
| `decimal128(p, s)` | `NUMERIC(p, s)` | ✅ <sup>[14](#fn14)</sup> | ✅ as text <sup>[12](#fn12)</sup> |
| `decimal32`, `decimal64`, `decimal256` | `NUMERIC(p, s)` | ❌ <sup>[15](#fn15)</sup> | ✅ as text <sup>[12](#fn12)</sup> |
| `list<T>`, `large_list<T>` | `ARRAY(T)` | ✅ | ❌ <sup>[5](#fn5)</sup> |
| `fixed_size_list<T>` | `ARRAY(T)` | ❌ <sup>[15](#fn15)</sup> | ❌ <sup>[5](#fn5)</sup> |
| `struct<…>` | `STRUCT(…)` | ✅ <sup>[3](#fn3)</sup> | ❌ <sup>[5](#fn5)</sup> |
| `dictionary<T>` | (value type) | ❌ <sup>[11](#fn11)</sup> | ✅ decoded as `T` |
| `map`, `null`, `duration`, `interval` | (no mapping) | ❌ | ❌ |

A `NULL` value binds as SQL `NULL` whatever its Arrow type.

### Footnotes

1. <a id="fn1"></a>Firebolt has no temporary tables.
2. <a id="fn2"></a>Set a target schema too; a catalog alone is read as the schema.
3. <a id="fn3"></a>Only top-level columns become `NOT NULL`; Firebolt requires `STRUCT` fields to be nullable.
4. <a id="fn4"></a>Prepare is local; `adbc_prepare()` asks the server for parameter types without running the query ([details](OPTIONS.md#parameter-metadata)).
5. <a id="fn5"></a>Binary and nested values cannot be bound as parameters; ingest them instead. Named parameters are always `TEXT` ([details](OPTIONS.md#query-parameters)).
6. <a id="fn6"></a>Firebolt does not report affected rows; use `SELECT count(*)`.
7. <a id="fn7"></a>Typed option setters, `Cancel`, `ExecuteSchema`, `GetStatistics`, `ErrorGetDetail`.
8. <a id="fn8"></a>WKB bytes, without GeoArrow metadata.
9. <a id="fn9"></a>Values above the signed 64-bit maximum are rejected.
10. <a id="fn10"></a>`NaN` and infinities are rejected.
11. <a id="fn11"></a>Not supported by the Arrow IPC writer; cast to `string`/`binary` or decode the dictionary first.
12. <a id="fn12"></a>Firebolt converts the text in most contexts; otherwise cast (`$1::DATE`).
13. <a id="fn13"></a>Stored as UTC; the original offset is not kept.
14. <a id="fn14"></a>Precision up to 38.
15. <a id="fn15"></a>Rejected by the server; cast to `decimal128` or `list`.

## Troubleshooting

| Error | Fix |
|-------|-----|
| `Could not connect to server` | Is the engine running? Check `curl -fsS http://localhost:3473/ping`. |
| `Cluster not yet healthy` | The engine is still starting; retry in a few seconds. |
| `Database 'uri' must start with …` | Include the scheme: `http://localhost:3473`, not `localhost:3473`. |
| `HTTP 401` / `403` | The engine requires authentication; see [docs/authentication.md](docs/authentication.md). |
| `No CA certificate bundle found` / `SSL peer certificate … was not OK` | Install your OS's `ca-certificates`, or set `firebolt.ssl_certificate_path` to your CA's PEM file. |
| `positional parameter $1, but it was not set` | Pass one value per `$N` placeholder; numbering starts at `$1`. |
| `current transaction is aborted` | A statement failed inside a transaction; call `conn.rollback()`. |
| `Unknown Firebolt database option` | Check the option name against [OPTIONS.md](OPTIONS.md). |

## Build from source

Only needed to work on the driver itself; otherwise use a
[release](https://github.com/adbc-drivers/firebolt/releases). Requires git,
Docker and [Pixi](https://pixi.sh/):

```bash
git submodule update --init --recursive
pixi run make   # → build/libadbc_driver_firebolt.{so,dylib,dll}
```

Testing and contribution guidelines are in [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

| | |
|---|---|
| [OPTIONS.md](OPTIONS.md) | Every option, error status and type mapping |
| [docs/authentication.md](docs/authentication.md) | Authentication: what works now and what is planned |
| [examples/python/](examples/python) | Runnable examples |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Building, testing and submitting changes |
| [CHANGELOG.md](CHANGELOG.md) | Release history |

## Other environments

FB2 only: engines (v5+) in a Firebolt 2.0 SaaS account — see [docs/fb2-saas.md](docs/fb2-saas.md).

## License

Apache 2.0 — see [LICENSE.txt](LICENSE.txt) and [NOTICE.txt](NOTICE.txt). Release
packages also include the licenses of all statically linked dependencies.
