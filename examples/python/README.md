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

# Python examples

Runnable scripts for the tasks in the [main README](../../README.md). Each one is
standalone — copy a single file out of here and it still works.

## Prerequisites

```bash
pip install adbc-driver-manager pyarrow
pip install pandas polars     # optional, for query_to_pandas.py
```

You also need the driver and an engine to talk to. Either download a
[release](https://github.com/firebolt-db/firebolt-adbc/releases/latest) or build
locally with `./scripts/build.sh`, then start an engine:

```bash
docker run -d --name firebolt -p 3473:3473 ghcr.io/firebolt-db/engine:latest
```

A freshly started engine has authentication disabled and a database named
`firebolt`, so no credentials are needed.

## Configuration

All examples read the same environment variables:

| Variable | Default | Meaning |
|----------|---------|---------|
| `FIREBOLT_ADBC_DRIVER` | `../../build/libfirebolt_adbc.so` | Path to the driver `.so`. Point this at your download. |
| `FIREBOLT_URI` | `http://localhost:3473` | Engine HTTP endpoint. Must be `http://` — this build has no TLS. |
| `FIREBOLT_DATABASE` | *(server default)* | Database name, if you need a specific one. |
| `FIREBOLT_TOKEN` | *(unset)* | Bearer token, only for an engine that requires one. |

With the defaults, and the driver built locally, the examples run as-is:

```bash
python examples/python/quickstart.py
```

Against a downloaded driver:

```bash
export FIREBOLT_ADBC_DRIVER=$PWD/libfirebolt_adbc-x86_64.so
python examples/python/quickstart.py
```

## The examples

| File | Shows |
|------|-------|
| [`quickstart.py`](quickstart.py) | Connect and query, through both the DBAPI wrapper and the low-level ADBC objects. Start here. |
| [`query_to_pandas.py`](query_to_pandas.py) | Results as `pyarrow.Table`, `pandas.DataFrame`, and `polars.DataFrame`; plus streaming a 100k-row result in batches instead of materialising it. |
| [`bulk_ingest.py`](bulk_ingest.py) | Uploading Arrow data: all four ingest modes, nested `ARRAY`/`STRUCT` columns with driver-generated DDL, and the low-level `bind_stream` path. |
| [`query_params.py`](query_params.py) | Binding values into a query: positional `$1`, `$2`, …, named `param('name')`, `executemany`, and the parameter schema from `adbc_prepare`. |

`bulk_ingest.py` and `query_params.py` create and drop tables named
`adbc_example_*`.

## Things worth knowing

- **Placeholders are `$1`, `$2`, …**, not `?` or `%s` — see
  [`query_params.py`](query_params.py). Values are substituted by the server into
  the parsed statement, so never format them into the SQL yourself.
- **`cursor.rowcount` is always `-1`.** The server does not report affected rows
  over this interface. Use `SELECT count(*)` when you need a number.
- **Pass `autocommit=True`** when a write has to be visible to a later read on a
  different connection, as `bulk_ingest.py` does.
- **Reserved words** are worth quoting in aliases; `rows`, for instance, is
  reserved and `AS rows` is a syntax error.
