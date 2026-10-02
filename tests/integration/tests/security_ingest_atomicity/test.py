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

"""Ingest atomicity regression test.

When ADBC_INGEST_OPTION_TARGET_TABLE is configured and the mode requires
DDL (replace / create / create_append) but no Arrow data has been bound,
the driver must NOT run the DDL.  Before the fix, mode=replace would
DROP the existing table and CREATE an empty replacement before the
INSERT inevitably failed — silently destroying the original.

These tests use the real Firebolt Core fixture so the table state can
be inspected via SQL after the failed ingest attempt.
"""

import adbc_driver_manager
import pyarrow as pa
import pytest


def _make_statement(conn):
    return adbc_driver_manager.AdbcStatement(conn)


def test_replace_without_bind_preserves_table(conn, run_query, table_name):
    """mode=replace + target_table + no bound data must leave the existing
    table intact, NOT dropped-and-recreated empty."""
    run_query(f'CREATE TABLE "{table_name}" (id INT, label TEXT)')
    try:
        run_query(f"INSERT INTO \"{table_name}\" VALUES (1, 'preexisting')")

        with _make_statement(conn) as stmt:
            stmt.set_options(
                **{
                    "adbc.ingest.target_table": table_name,
                    "adbc.ingest.mode": "adbc.ingest.mode.replace",
                }
            )
            # Deliberately do NOT bind data.
            with pytest.raises(adbc_driver_manager.ProgrammingError):
                stmt.execute_update()

        # The pre-existing row must still be there.
        result = run_query(f'SELECT id, label FROM "{table_name}" ORDER BY id')
        assert result.num_rows == 1, (
            f"replace-without-bind destroyed the table: {result.to_pylist()}"
        )
        assert result["id"][0].as_py() == 1
        assert result["label"][0].as_py() == "preexisting"
    finally:
        run_query(f'DROP TABLE IF EXISTS "{table_name}"')


def test_create_without_bind_does_not_create_table(conn, run_query, table_name):
    """mode=create + target_table + no bound data must not create the table."""
    # Pre-condition: the table does not exist yet.
    run_query(f'DROP TABLE IF EXISTS "{table_name}"')

    with _make_statement(conn) as stmt:
        stmt.set_options(
            **{
                "adbc.ingest.target_table": table_name,
                "adbc.ingest.mode": "adbc.ingest.mode.create",
            }
        )
        with pytest.raises(adbc_driver_manager.ProgrammingError):
            stmt.execute_update()

    # The table must still not exist — i.e. SELECT must fail with a "not found"
    # style error, not return zero rows from a successful CREATE.
    with pytest.raises(adbc_driver_manager.ProgrammingError):
        run_query(f'SELECT * FROM "{table_name}" LIMIT 0')


def test_failed_ingest_does_not_carry_bytes_to_next_query(conn_to_mock, mock_server):
    """Bound Arrow bytes from a failed ingest must not silently re-attach to
    the next query on the same statement.  Drives an ingest against the
    mock server (which fails the request), then issues a plain SELECT and
    asserts the second request is not a multipart upload."""
    table = pa.table({"x": pa.array([1, 2, 3], type=pa.int32())})

    # Step 1: queue a failure for the ingest attempt.
    mock_server.queue(status=500, body=b"upstream error")

    with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
        stmt.set_options(
            **{
                "adbc.ingest.target_table": "tgt",
                "adbc.ingest.mode": "adbc.ingest.mode.append",
            }
        )
        stmt.bind_stream(table.to_reader())
        with pytest.raises(adbc_driver_manager.OperationalError):
            stmt.execute_update()

        # Step 2: issue a plain SELECT on the SAME statement.  The mock
        # server returns 200/empty, but we only care about the request shape.
        stmt.set_sql_query("SELECT 1")
        stmt.execute_query()

    # The second request must not be multipart (which would mean bound bytes
    # rode along).  set_sql_query also clears ingest state, so the second
    # request body is the plain SQL text — no `multipart/form-data` header.
    assert len(mock_server.captured) >= 2
    second_ct = mock_server.captured[1].headers.get("Content-Type", "")
    assert "multipart" not in second_ct.lower(), (
        f"bound bytes attached to next query: Content-Type={second_ct}"
    )


def test_append_with_bind_still_works(conn, run_query, temp_table):
    """Sanity: legitimate ingest path (bind + execute_update) remains green."""
    table = pa.table(
        {
            "id": pa.array([1, 2, 3], type=pa.int32()),
            "label": pa.array(["a", "b", "c"]),
            "value": pa.array([1.0, 2.0, 3.0]),
        }
    )

    with _make_statement(conn) as stmt:
        stmt.set_options(
            **{
                "adbc.ingest.target_table": temp_table,
                "adbc.ingest.mode": "adbc.ingest.mode.append",
            }
        )
        stmt.bind_stream(table.to_reader())
        stmt.execute_update()

    result = run_query(f'SELECT COUNT(*) AS n FROM "{temp_table}"')
    assert result["n"][0].as_py() == 3
