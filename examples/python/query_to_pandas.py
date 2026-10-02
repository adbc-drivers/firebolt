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

"""Get query results into pyarrow, pandas, and polars, and stream a big one.

    python examples/python/query_to_pandas.py

Configure with FIREBOLT_ADBC_DRIVER / FIREBOLT_URI — see examples/python/README.md.
"""

import argparse
import os

from adbc_driver_manager import dbapi

DEFAULT_DRIVER = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "libadbc_driver_firebolt.so"
)


def connection_settings() -> tuple[str, dict]:
    driver = os.environ.get("FIREBOLT_ADBC_DRIVER", DEFAULT_DRIVER)
    db_kwargs = {"uri": os.environ.get("FIREBOLT_URI", "http://localhost:3473")}
    if os.environ.get("FIREBOLT_DATABASE"):
        db_kwargs["adbc.firebolt.database"] = os.environ["FIREBOLT_DATABASE"]
    if os.environ.get("FIREBOLT_TOKEN"):
        db_kwargs["adbc.firebolt.token"] = os.environ["FIREBOLT_TOKEN"]
    return driver, db_kwargs


QUERY = """
SELECT
    n                        AS id,
    n * 1.5                  AS score,
    'row-' || n::TEXT        AS label,
    n % 2 = 0                AS is_even
FROM generate_series(1, 5) AS s(n)
"""


def main() -> None:
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args()
    driver, db_kwargs = connection_settings()

    with dbapi.connect(driver=driver, db_kwargs=db_kwargs) as conn:
        with conn.cursor() as cur:
            # --- pyarrow.Table: the native shape, no conversion cost ---------
            cur.execute(QUERY)
            table = cur.fetch_arrow_table()
            print("=== pyarrow.Table ===")
            print(table)
            print(f"\nschema:\n{table.schema}")

            # --- pandas -----------------------------------------------------
            cur.execute(QUERY)
            print("\n=== pandas.DataFrame ===")
            print(cur.fetch_df())

            # --- polars: zero-copy from the same Arrow table -----------------
            try:
                import polars as pl
            except ImportError:
                print("\n(polars not installed; skipping)")
            else:
                cur.execute(QUERY)
                print("\n=== polars.DataFrame ===")
                print(pl.from_arrow(cur.fetch_arrow_table()))

            # --- streaming --------------------------------------------------
            # fetch_arrow_table() materialises the whole result.  For anything
            # that might not fit in memory, iterate record batches instead:
            # the driver hands them over as the HTTP response arrives.
            print("\n=== streaming batches ===")
            cur.execute("SELECT n FROM generate_series(1, 100000) AS s(n)")
            rows = 0
            batches = 0
            for batch in cur.fetch_record_batch():
                rows += batch.num_rows
                batches += 1
            print(f"consumed {rows} rows in {batches} batch(es) without holding the result")


if __name__ == "__main__":
    main()
