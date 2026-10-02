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

"""
Bulk ingest tests — canonical / public-API path.

Exercises the high-level `dbapi.Cursor.adbc_ingest()` wrapper that ADBC
clients are expected to use.  The wrapper handles option setup, bind-path
selection (RecordBatch → bind, Table / RecordBatchReader → bind_stream),
and end-of-stream handling internally; these tests pin the driver against
the full set of options and bind paths that the wrapper sends.

The direct AdbcStatement tests (set_options + bind_stream + execute_update)
live in tests/ingest_low_level/.
"""

from decimal import Decimal

import adbc_driver_manager
import pyarrow as pa
import pytest
from adbc_driver_manager import dbapi

# --------------------------------------------------------------------------- #
# Helpers  (the `cursor` / `dbapi_conn` fixtures live in conftest.py)          #
# --------------------------------------------------------------------------- #


def _row_count(run_query, table_name):
    return run_query(f"SELECT COUNT(*) AS n FROM {table_name}")["n"][0].as_py()


# --------------------------------------------------------------------------- #
# Mode coverage                                                               #
# --------------------------------------------------------------------------- #


class TestModes:
    """One test per ingest mode supported by adbc_ingest()."""

    def test_append(self, cursor, run_query, temp_table) -> None:
        tbl = pa.table(
            {
                "id": pa.array([1, 2, 3], type=pa.int32()),
                "label": ["a", "b", "c"],
                "value": [1.0, 2.0, 3.0],
            }
        )
        cursor.adbc_ingest(temp_table, tbl, mode="append")
        assert _row_count(run_query, temp_table) == 3

    def test_append_extends_existing(self, cursor, run_query, temp_table) -> None:
        run_query(f"INSERT INTO {temp_table} VALUES (0, 'pre', 0.0)")
        tbl = pa.table(
            {
                "id": pa.array([1, 2], type=pa.int32()),
                "label": ["a", "b"],
                "value": [1.0, 2.0],
            }
        )
        cursor.adbc_ingest(temp_table, tbl, mode="append")
        assert _row_count(run_query, temp_table) == 3

    def test_append_into_missing_table_fails(self, cursor, table_name) -> None:
        tbl = pa.table({"x": pa.array([1, 2], type=pa.int32())})
        with pytest.raises(dbapi.ProgrammingError):
            cursor.adbc_ingest(table_name, tbl, mode="append")

    def test_create(self, cursor, run_query, table_name) -> None:
        try:
            tbl = pa.table(
                {
                    "id": pa.array([10, 20], type=pa.int32()),
                    "label": ["foo", "bar"],
                }
            )
            cursor.adbc_ingest(table_name, tbl, mode="create")
            assert _row_count(run_query, table_name) == 2
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_create_existing_table_fails(self, cursor, temp_table) -> None:
        # temp_table fixture already created the table — create mode must fail.
        tbl = pa.table(
            {
                "id": pa.array([1], type=pa.int32()),
                "label": ["x"],
                "value": [1.0],
            }
        )
        with pytest.raises(dbapi.ProgrammingError):
            cursor.adbc_ingest(temp_table, tbl, mode="create")

    def test_replace(self, cursor, run_query, table_name) -> None:
        try:
            run_query(f"CREATE TABLE {table_name} (id INT)")
            run_query(f"INSERT INTO {table_name} VALUES (1)")
            # New schema differs — replace must DROP and recreate.
            tbl = pa.table(
                {
                    "id": pa.array([5, 6], type=pa.int32()),
                    "tag": ["p", "q"],
                }
            )
            cursor.adbc_ingest(table_name, tbl, mode="replace")
            t = run_query(f"SELECT id, tag FROM {table_name} ORDER BY id")
            assert t["id"].to_pylist() == [5, 6]
            assert t["tag"].to_pylist() == ["p", "q"]
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_replace_with_the_same_schema(self, cursor, run_query, table_name) -> None:
        # Schema matches the existing table — replace must still wipe the
        # pre-existing row (1) and leave only the freshly ingested rows.
        try:
            run_query(f"CREATE TABLE {table_name} (id INT)")
            run_query(f"INSERT INTO {table_name} VALUES (1)")
            tbl = pa.table({"id": pa.array([5, 6], type=pa.int32())})
            cursor.adbc_ingest(table_name, tbl, mode="replace")
            t = run_query(f"SELECT id FROM {table_name} ORDER BY id")
            assert t["id"].to_pylist() == [5, 6]
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_replace_creates_when_missing(self, cursor, run_query, table_name) -> None:
        try:
            tbl = pa.table({"id": pa.array([7], type=pa.int32())})
            cursor.adbc_ingest(table_name, tbl, mode="replace")
            assert _row_count(run_query, table_name) == 1
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_create_append(self, cursor, run_query, table_name) -> None:
        try:
            tbl = pa.table(
                {
                    "id": pa.array([1, 2], type=pa.int32()),
                    "v": pa.array([10, 20], type=pa.int32()),
                }
            )
            cursor.adbc_ingest(table_name, tbl, mode="create_append")
            assert _row_count(run_query, table_name) == 2

            more = pa.table(
                {
                    "id": pa.array([3], type=pa.int32()),
                    "v": pa.array([30], type=pa.int32()),
                }
            )
            cursor.adbc_ingest(table_name, more, mode="create_append")
            assert _row_count(run_query, table_name) == 3
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_create_append_on_existing_mismatched_schema_fails(
        self, cursor, run_query, table_name
    ) -> None:
        # create_append on an existing table must append, not recreate — so a
        # schema that doesn't line up with the existing columns has to error
        try:
            run_query(f"CREATE TABLE {table_name} (id INT)")
            tbl = pa.table({"name": ["a", "b"]})
            with pytest.raises(
                Exception, match="Column .* does not exist in the target INSERT table"
            ):
                cursor.adbc_ingest(table_name, tbl, mode="create_append")
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")


# --------------------------------------------------------------------------- #
# Bind paths: RecordBatch / Table / RecordBatchReader                         #
# --------------------------------------------------------------------------- #


class TestBindPaths:
    """The dbapi wrapper picks one of three bind paths based on the input
    object's __arrow_c_array__ / __arrow_c_stream__ protocol; each path must
    work end-to-end."""

    def test_table_uses_bind_stream(self, cursor, run_query, table_name) -> None:
        # pa.Table → __arrow_c_stream__ → AdbcStatement.bind_stream.
        try:
            tbl = pa.table(
                {
                    "id": pa.array([1, 2, 3], type=pa.int32()),
                    "label": ["alpha", "beta", "gamma"],
                }
            )
            cursor.adbc_ingest(table_name, tbl, mode="create")
            cursor.execute(f"SELECT id, label FROM {table_name} ORDER BY id")
            got = cursor.fetch_arrow_table()
            assert got.column("id").to_pylist() == [1, 2, 3]
            assert got.column("label").to_pylist() == ["alpha", "beta", "gamma"]
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_record_batch_uses_bind(self, cursor, run_query, table_name) -> None:
        # pa.RecordBatch → __arrow_c_array__ → AdbcStatement.bind (single batch).
        try:
            batch = pa.record_batch(
                {
                    "id": pa.array([7, 8], type=pa.int32()),
                    "v": pa.array([0.5, 0.75], type=pa.float64()),
                }
            )
            cursor.adbc_ingest(table_name, batch, mode="create")
            cursor.execute(f"SELECT id, v FROM {table_name} ORDER BY id")
            got = cursor.fetch_arrow_table()
            assert got.column("id").to_pylist() == [7, 8]
            assert got.column("v").to_pylist() == [0.5, 0.75]
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_record_batch_reader_multi_batch(
        self, cursor, run_query, table_name
    ) -> None:
        # Multiple record batches in a RecordBatchReader → one Arrow IPC stream
        # uploaded to Firebolt in a single multipart HTTP request.
        try:
            schema = pa.schema([("id", pa.int32()), ("name", pa.string())])
            b1 = pa.record_batch(
                {"id": pa.array([1, 2], type=pa.int32()), "name": ["a", "b"]}
            )
            b2 = pa.record_batch({"id": pa.array([3], type=pa.int32()), "name": ["c"]})
            b3 = pa.record_batch(
                {"id": pa.array([4, 5], type=pa.int32()), "name": ["d", "e"]}
            )
            reader = pa.RecordBatchReader.from_batches(schema, [b1, b2, b3])

            cursor.adbc_ingest(table_name, reader, mode="create")
            cursor.execute(f"SELECT id, name FROM {table_name} ORDER BY id")
            got = cursor.fetch_arrow_table()
            assert got.column("id").to_pylist() == [1, 2, 3, 4, 5]
            assert got.column("name").to_pylist() == ["a", "b", "c", "d", "e"]
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")


# --------------------------------------------------------------------------- #
# Empty-blob ingest                                                           #
# --------------------------------------------------------------------------- #


class TestEmptyData:
    """Bind sources that carry a valid schema but zero rows.

    The Arrow IPC writer always emits a schema message at the start of the
    stream, so the multipart upload body is non-empty even when no record
    batches are written — these tests pin that the driver routes correctly
    through executeInsert and the server tolerates a row-less payload."""

    def test_empty_table_append(self, cursor, run_query, temp_table) -> None:
        # Existing table, zero-row Table → append must be a no-op, not error.
        empty = pa.table(
            {
                "id": pa.array([], type=pa.int32()),
                "label": pa.array([], type=pa.string()),
                "value": pa.array([], type=pa.float64()),
            }
        )
        cursor.adbc_ingest(temp_table, empty, mode="append")
        assert _row_count(run_query, temp_table) == 0

    def test_empty_table_create(self, cursor, run_query, table_name) -> None:
        # Zero-row Table → create must still produce the table with the right
        # schema, just empty.
        try:
            empty = pa.table(
                {
                    "id": pa.array([], type=pa.int32()),
                    "label": pa.array([], type=pa.string()),
                }
            )
            cursor.adbc_ingest(table_name, empty, mode="create")
            assert _row_count(run_query, table_name) == 0
            schema = run_query(f"SELECT id, label FROM {table_name} LIMIT 0").schema
            assert schema.field("id").type == pa.int32()
            assert schema.field("label").type == pa.string()
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_zero_batch_reader_create(self, cursor, run_query, table_name) -> None:
        # RecordBatchReader with zero batches: the IPC stream has only the
        # schema message.  Driver must still pick the multipart path and
        # create the table.
        try:
            schema = pa.schema([("id", pa.int32()), ("name", pa.string())])
            reader = pa.RecordBatchReader.from_batches(schema, [])
            cursor.adbc_ingest(table_name, reader, mode="create")
            assert _row_count(run_query, table_name) == 0
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")


# --------------------------------------------------------------------------- #
# Cursor reuse: ingest + read-back through the same cursor                    #
# --------------------------------------------------------------------------- #


class TestCursorReuse:
    """Common workflows that reuse the same cursor across multiple ingests
    and queries.  Pins down statement-state cleanup between calls."""

    def test_append_after_create_via_cursor(
        self, cursor, run_query, table_name
    ) -> None:
        try:
            cursor.adbc_ingest(
                table_name,
                pa.table({"id": pa.array([1, 2], type=pa.int32())}),
                mode="create",
            )
            cursor.adbc_ingest(
                table_name,
                pa.table({"id": pa.array([3, 4], type=pa.int32())}),
                mode="append",
            )
            cursor.execute(f"SELECT COUNT(*) AS n FROM {table_name}")
            assert cursor.fetch_arrow_table().column("n")[0].as_py() == 4
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_replace_then_query_via_same_cursor(
        self, cursor, run_query, table_name
    ) -> None:
        try:
            cursor.adbc_ingest(
                table_name,
                pa.table({"id": pa.array([100], type=pa.int32()), "x": ["old"]}),
                mode="create",
            )
            cursor.adbc_ingest(
                table_name,
                pa.table({"id": pa.array([1, 2], type=pa.int32()), "y": [1.5, 2.5]}),
                mode="replace",
            )
            cursor.execute(f"SELECT id, y FROM {table_name} ORDER BY id")
            got = cursor.fetch_arrow_table()
            assert got.column_names == ["id", "y"]
            assert got.column("id").to_pylist() == [1, 2]
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_cursor_rowcount_reflects_ingest(
        self, cursor, run_query, table_name
    ) -> None:
        # adbc_ingest() returns the row count (or -1).  Most drivers return -1
        # because the HTTP API doesn't surface inserted-row totals; the call
        # must not raise for a valid ingest.
        try:
            tbl = pa.table({"id": pa.array([1, 2, 3], type=pa.int32())})
            rc = cursor.adbc_ingest(table_name, tbl, mode="create")
            assert isinstance(rc, int)
            cursor.execute(f"SELECT COUNT(*) AS n FROM {table_name}")
            assert cursor.fetch_arrow_table().column("n")[0].as_py() == 3
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")


# --------------------------------------------------------------------------- #
# Type coverage                                                               #
# --------------------------------------------------------------------------- #


class TestTypes:
    """The CREATE TABLE DDL is synthesised from the bound Arrow schema; each
    Arrow type that maps to a Firebolt SQL type needs an end-to-end check."""

    def _ingest_and_drop(self, cursor, run_query, table_name, tbl):
        try:
            cursor.adbc_ingest(table_name, tbl, mode="create")
            assert _row_count(run_query, table_name) == tbl.num_rows
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_int_and_string(self, cursor, run_query, table_name) -> None:
        tbl = pa.table(
            {
                "id": pa.array([1, 2], type=pa.int32()),
                "name": ["alpha", "beta"],
            }
        )
        self._ingest_and_drop(cursor, run_query, table_name, tbl)

    def test_int64_and_double(self, cursor, run_query, table_name) -> None:
        tbl = pa.table(
            {
                "id": pa.array([2**40, 2**41], type=pa.int64()),
                "v": pa.array([1.5, 2.5], type=pa.float64()),
            }
        )
        self._ingest_and_drop(cursor, run_query, table_name, tbl)

    def test_bool(self, cursor, run_query, table_name) -> None:
        tbl = pa.table(
            {
                "id": pa.array([1, 2], type=pa.int32()),
                "flag": pa.array([True, False], type=pa.bool_()),
            }
        )
        self._ingest_and_drop(cursor, run_query, table_name, tbl)

    def test_decimal(self, cursor, run_query, table_name) -> None:
        tbl = pa.table(
            {
                "id": pa.array([1, 2], type=pa.int32()),
                "amount": pa.array(
                    [Decimal("1.23"), Decimal("4.56")], type=pa.decimal128(10, 2)
                ),
            }
        )
        self._ingest_and_drop(cursor, run_query, table_name, tbl)

    def test_array(self, cursor, run_query, table_name) -> None:
        tbl = pa.table(
            {
                "id": pa.array([1, 2], type=pa.int32()),
                "tags": pa.array([[1, 2, 3], [4]], type=pa.list_(pa.int32())),
            }
        )
        self._ingest_and_drop(cursor, run_query, table_name, tbl)


# --------------------------------------------------------------------------- #
# Qualified target / unsupported options                                      #
# --------------------------------------------------------------------------- #


class TestOptions:
    def test_qualified_target_db_schema_passes_through(
        self, cursor, run_query, table_name
    ) -> None:
        # db_schema_name='public' must produce a schema-qualified table reference
        # in the generated DDL/INSERT.  Firebolt Core exposes the default
        # 'public' schema, so the round-trip should still work.
        try:
            cursor.adbc_ingest(
                table_name,
                pa.table({"id": pa.array([42], type=pa.int32())}),
                mode="create",
                db_schema_name="public",
            )
            cursor.execute(f'SELECT id FROM "public"."{table_name}"')
            got = cursor.fetch_arrow_table()
            assert got.column("id").to_pylist() == [42]
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_temporary_true_raises_not_supported(self, cursor, table_name) -> None:
        # Firebolt has no notion of session-temporary tables.  The driver must
        # surface NotSupportedError so dbapi callers can react to it explicitly.
        tbl = pa.table({"id": pa.array([1], type=pa.int32())})
        with pytest.raises(
            (dbapi.NotSupportedError, adbc_driver_manager.NotSupportedError)
        ):
            cursor.adbc_ingest(table_name, tbl, mode="create", temporary=True)
