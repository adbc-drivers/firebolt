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

"""Bind values into a query instead of formatting them into the SQL.

Firebolt's placeholders are positional: $1, $2, … — not ? or %s.  Values travel beside
the statement and the server substitutes them into the parsed query, so a value
containing quotes or SQL keywords is data.

    python examples/python/query_params.py

Configure with FIREBOLT_ADBC_DRIVER / FIREBOLT_URI — see examples/python/README.md.
"""

import argparse
import datetime
import decimal
import os

from adbc_driver_manager import dbapi

DEFAULT_DRIVER = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..",
    "..",
    "build",
    "libadbc_driver_firebolt.so",
)

TABLE = "adbc_example_params"


def connection_settings() -> tuple[str, dict]:
    """Driver path plus the database options, read from the environment."""
    driver = os.environ.get("FIREBOLT_ADBC_DRIVER", DEFAULT_DRIVER)
    db_kwargs = {"uri": os.environ.get("FIREBOLT_URI", "http://localhost:3473")}
    if os.environ.get("FIREBOLT_DATABASE"):
        db_kwargs["firebolt.database"] = os.environ["FIREBOLT_DATABASE"]
    if os.environ.get("FIREBOLT_TOKEN"):
        db_kwargs["firebolt.token"] = os.environ["FIREBOLT_TOKEN"]
    return driver, db_kwargs


def positional(cur) -> None:
    """$1, $2, … in bound-column order."""
    cur.execute("SELECT $1 + 1 AS result", (41,))
    print("arithmetic on a parameter:", cur.fetchone()[0])

    # Each value keeps its type: an integer arrives as BIGINT, a float as DOUBLE, a
    # bool as BOOLEAN, None as an untyped NULL.
    cur.execute(
        "SELECT $1 AS i, $2 AS f, $3 AS b, $4 IS NULL AS n", (7, 1.5, True, None)
    )
    print("types preserved:      ", cur.fetch_arrow_table().schema.types)

    # The same placeholder can be used more than once.
    cur.execute("SELECT $1 || '-' || $1 AS doubled", ("ab",))
    print("reused placeholder:   ", cur.fetchone()[0])


def not_string_formatting(cur) -> None:
    """Why binding is worth the trouble."""
    hostile = "'; DROP TABLE " + TABLE + "; --"

    cur.execute(f"SELECT label FROM {TABLE} WHERE label = $1", (hostile,))
    print("hostile value matched:", cur.fetchall())

    # Still there: the value never reached the parser as syntax.
    cur.execute(f"SELECT count(*) FROM {TABLE}")
    print("table intact, rows:   ", cur.fetchone()[0])


def text_typed_values(cur) -> None:
    """Dates, timestamps and decimals travel as exact text.

    The format carries only NULL, booleans, numbers and strings, so these go in
    canonical text form; Firebolt coerces them in most contexts, and a cast covers the
    rest.  A decimal is never sent as a float, so it keeps every digit.
    """
    cur.execute("SELECT $1::DATE AS d", (datetime.date(2024, 1, 5),))
    print("date:                 ", cur.fetchone()[0])

    # A naive datetime: TIMESTAMP carries no time zone (TIMESTAMPTZ would).
    ts = datetime.datetime(2024, 1, 5, 6, 7, 8, 123456)  # noqa: DTZ001
    cur.execute("SELECT $1::TIMESTAMP AS ts", (ts,))
    print("timestamp:            ", cur.fetchone()[0])

    exact = decimal.Decimal("-12345678901234567890.123456789")
    cur.execute("SELECT $1::DECIMAL(38, 9) AS d", (exact,))
    print("decimal round-trips:  ", cur.fetchone()[0] == exact)


def named(cur) -> None:
    """Pass a dict and read the values back through Firebolt's param() function.

    There is no named *placeholder* ($foo lexes as an identifier), and param() always
    yields TEXT, so cast for anything else.  Use one style per statement: a dict binds
    $1, $2, … as well, but to its keys in insertion order, which is not something to
    rely on.
    """
    cur.execute("SELECT param('who') AS who", {"who": "ann"})
    print("named parameter:      ", cur.fetchone()[0])

    cur.execute("SELECT param('cutoff')::INT + 1 AS n", {"cutoff": 41})
    print("named, cast to INT:   ", cur.fetchone()[0])

    cur.execute(
        "SELECT param('who') AS who, param('cutoff')::INT AS cutoff",
        {"who": "ann", "cutoff": 41},
    )
    print("several names:        ", cur.fetchone())


def many_rows(cur) -> None:
    """executemany runs the statement once per parameter set."""
    rows = [(i, f"row{i}") for i in range(5)]
    cur.executemany(f"INSERT INTO {TABLE} VALUES ($1, $2)", rows)

    cur.execute(f"SELECT id, label FROM {TABLE} WHERE id < $1 ORDER BY id", (3,))
    print("inserted and filtered:", cur.fetchall())


def parameter_schema(cur) -> None:
    """Ask the server what the placeholders are, without running the statement.

    Prepare itself costs nothing — Firebolt has no server-side prepare — so this is
    the one round-trip involved.  It is not cached: DDL can change the answer.
    """
    schema = cur.adbc_prepare(f"SELECT label FROM {TABLE} WHERE id = $1 AND label = $2")
    for field in schema:
        print(f"parameter {field.name}:          {field.type}")


def main() -> None:
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args()

    driver, db_kwargs = connection_settings()
    print(f"driver: {driver}\nuri:    {db_kwargs['uri']}\n")

    with (
        dbapi.connect(driver=driver, db_kwargs=db_kwargs, autocommit=True) as conn,
        conn.cursor() as cur,
    ):
        cur.execute(f"DROP TABLE IF EXISTS {TABLE}")
        cur.execute(f"CREATE TABLE {TABLE} (id INT, label TEXT)")
        try:
            print("=== positional parameters ===")
            positional(cur)

            print("\n=== values are never spliced into the SQL ===")
            not_string_formatting(cur)

            print("\n=== dates, timestamps, decimals ===")
            text_typed_values(cur)

            print("\n=== named parameters ===")
            named(cur)

            print("\n=== one execution per parameter set ===")
            many_rows(cur)

            print("\n=== parameter schema ===")
            parameter_schema(cur)
        finally:
            cur.execute(f"DROP TABLE IF EXISTS {TABLE}")


if __name__ == "__main__":
    main()
