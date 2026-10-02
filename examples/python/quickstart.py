#!/usr/bin/env python3
# Copyright (c) 2026 ADBC Drivers Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Connect to a Firebolt engine and run a query, two ways.

The dbapi path is what you want almost always.  The low-level path is shown
because it is the one you need for anything the DBAPI wrapper does not expose.

    python examples/python/quickstart.py

Configure with FIREBOLT_ADBC_DRIVER / FIREBOLT_URI — see examples/python/README.md.
"""

import argparse
import os

import adbc_driver_manager
import pyarrow as pa
from adbc_driver_manager import dbapi

DEFAULT_DRIVER = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..",
    "..",
    "build",
    "libadbc_driver_firebolt.so",
)


def connection_settings() -> tuple[str, dict]:
    """Driver path plus the database options, read from the environment."""
    driver = os.environ.get("FIREBOLT_ADBC_DRIVER", DEFAULT_DRIVER)
    db_kwargs = {"uri": os.environ.get("FIREBOLT_URI", "http://localhost:3473")}
    if os.environ.get("FIREBOLT_DATABASE"):
        db_kwargs["firebolt.database"] = os.environ["FIREBOLT_DATABASE"]
    # Only for an engine that requires a bearer token.  Leave FIREBOLT_TOKEN
    # unset against an engine with authentication disabled.
    if os.environ.get("FIREBOLT_TOKEN"):
        db_kwargs["firebolt.token"] = os.environ["FIREBOLT_TOKEN"]
    return driver, db_kwargs


def via_dbapi(driver: str, db_kwargs: dict) -> None:
    """The PEP 249 interface: connect, cursor, execute, fetch."""
    with (
        dbapi.connect(driver=driver, db_kwargs=db_kwargs) as conn,
        conn.cursor() as cur,
    ):
        cur.execute("SELECT 1 AS n, 'hello' AS greeting")
        print(cur.fetch_arrow_table())

        # Every result is Arrow underneath, so a plain fetchall() works too.
        cur.execute("SELECT 2 + 3 AS sum, upper('firebolt') AS name")
        print(cur.fetchall())

        info = conn.adbc_get_info()
        print(
            f"\nconnected to {info['vendor_name']} "
            f"via {info['driver_name']} {info['driver_version']}"
        )


def via_low_level(driver: str, db_kwargs: dict) -> None:
    """The C-API objects the DBAPI layer wraps.

    Worth knowing because the statement object is where the ingest options and
    bind_stream live (see bulk_ingest.py), and because it hands you the Arrow
    stream directly with no copy.
    """
    with (
        adbc_driver_manager.AdbcDatabase(driver=driver, **db_kwargs) as db,
        adbc_driver_manager.AdbcConnection(db) as conn,
        adbc_driver_manager.AdbcStatement(conn) as stmt,
    ):
        stmt.set_sql_query("SELECT 42 AS answer")
        stream, _rows = stmt.execute_query()
        print(pa.RecordBatchReader.from_stream(stream).read_all())


def main() -> None:
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args()

    driver, db_kwargs = connection_settings()
    print(f"driver: {driver}\nuri:    {db_kwargs['uri']}\n")

    print("=== via adbc_driver_manager.dbapi ===")
    via_dbapi(driver, db_kwargs)

    print("\n=== via the low-level ADBC objects ===")
    via_low_level(driver, db_kwargs)


if __name__ == "__main__":
    main()
