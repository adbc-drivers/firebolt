"""
Low-level bulk ingest tests.

Drives the ADBC bulk-ingest API by calling AdbcStatement.set_options /
bind_stream / execute_update directly — bypassing the high-level
dbapi.Cursor.adbc_ingest() wrapper.  Covers each ingest mode end-to-end and
the Arrow → Firebolt SQL type mapping for the create-style modes.

The canonical/public-API tests (cursor.adbc_ingest) live in tests/ingest/.
"""

from decimal import Decimal

import adbc_driver_manager
import pyarrow as pa
import pytest


# --------------------------------------------------------------------------- #
# Helpers                                                                     #
# --------------------------------------------------------------------------- #

def _ingest(conn, table_name, data, mode, *, catalog=None, db_schema=None):
    """Drive bulk-ingest directly via AdbcStatement, mirroring what the dbapi
    Cursor.adbc_ingest() wrapper does internally.  Used to pin the C++ driver
    against the option set the high-level wrapper sends."""
    options = {
        adbc_driver_manager.StatementOptions.INGEST_MODE.value: mode,
        adbc_driver_manager.StatementOptions.INGEST_TARGET_TABLE.value: table_name,
    }
    if catalog is not None:
        options[adbc_driver_manager.StatementOptions.INGEST_TARGET_CATALOG.value] = catalog
    if db_schema is not None:
        options[adbc_driver_manager.StatementOptions.INGEST_TARGET_DB_SCHEMA.value] = db_schema

    with adbc_driver_manager.AdbcStatement(conn) as stmt:
        stmt.set_options(**options)
        # AdbcStatement.bind_stream accepts the ArrowArrayStream PyCapsule
        # produced by pyarrow's __arrow_c_stream__ method.
        stmt.bind_stream(data.__arrow_c_stream__())
        stmt.execute_update()


def _row_count(run_query, table_name):
    return run_query(f"SELECT COUNT(*) AS n FROM {table_name}")["n"][0].as_py()


# --------------------------------------------------------------------------- #
# Mode: append                                                                #
# --------------------------------------------------------------------------- #

class TestAppendMode:
    def test_append_basic(self, conn, run_query, temp_table) -> None:
        tbl = pa.table({
            "id": pa.array([1, 2, 3], type=pa.int32()),
            "label": ["alpha", "beta", "gamma"],
            "value": [1.5, 2.5, 3.5],
        })
        _ingest(conn, temp_table, tbl, "adbc.ingest.mode.append")
        assert _row_count(run_query, temp_table) == 3

    def test_append_extends_existing(self, conn, run_query, temp_table) -> None:
        run_query(f"INSERT INTO {temp_table} VALUES (0, 'pre', 0.0)")
        tbl = pa.table({
            "id": pa.array([1, 2], type=pa.int32()),
            "label": ["a", "b"],
            "value": [1.0, 2.0],
        })
        _ingest(conn, temp_table, tbl, "adbc.ingest.mode.append")
        assert _row_count(run_query, temp_table) == 3

    def test_append_into_missing_table_fails(self, conn, table_name) -> None:
        tbl = pa.table({"x": pa.array([1, 2], type=pa.int32())})
        with pytest.raises(Exception):
            _ingest(conn, table_name, tbl, "adbc.ingest.mode.append")


# --------------------------------------------------------------------------- #
# Mode: create                                                                #
# --------------------------------------------------------------------------- #

class TestCreateMode:
    def test_create_new_table(self, conn, run_query, table_name) -> None:
        try:
            tbl = pa.table({
                "id": pa.array([10, 20], type=pa.int32()),
                "name": ["foo", "bar"],
            })
            _ingest(conn, table_name, tbl, "adbc.ingest.mode.create")
            t = run_query(f"SELECT id, name FROM {table_name} ORDER BY id")
            assert t["id"].to_pylist() == [10, 20]
            assert t["name"].to_pylist() == ["foo", "bar"]
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_create_existing_table_fails(self, conn, run_query, temp_table) -> None:
        # temp_table fixture already created the table — create mode must fail.
        tbl = pa.table({
            "id": pa.array([1], type=pa.int32()),
            "label": ["x"],
            "value": [1.0],
        })
        with pytest.raises(Exception):
            _ingest(conn, temp_table, tbl, "adbc.ingest.mode.create")


# --------------------------------------------------------------------------- #
# Mode: replace                                                               #
# --------------------------------------------------------------------------- #

class TestReplaceMode:
    def test_replace_drops_and_recreates(self, conn, run_query, temp_table) -> None:
        run_query(f"INSERT INTO {temp_table} VALUES (99, 'old', 99.0)")
        # New schema differs — replace must DROP first.
        tbl = pa.table({
            "id": pa.array([1, 2], type=pa.int32()),
            "tag": ["new1", "new2"],
        })
        _ingest(conn, temp_table, tbl, "adbc.ingest.mode.replace")
        t = run_query(f"SELECT id, tag FROM {temp_table} ORDER BY id")
        assert t.column_names == ["id", "tag"]
        assert t["id"].to_pylist() == [1, 2]
        assert t["tag"].to_pylist() == ["new1", "new2"]

    def test_replace_creates_when_missing(self, conn, run_query, table_name) -> None:
        try:
            tbl = pa.table({"id": pa.array([7], type=pa.int32())})
            _ingest(conn, table_name, tbl, "adbc.ingest.mode.replace")
            assert _row_count(run_query, table_name) == 1
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")


# --------------------------------------------------------------------------- #
# Mode: create_append                                                         #
# --------------------------------------------------------------------------- #

class TestCreateAppendMode:
    def test_create_append_new_table(self, conn, run_query, table_name) -> None:
        try:
            tbl = pa.table({
                "id": pa.array([1, 2, 3], type=pa.int32()),
                "v": pa.array([10, 20, 30], type=pa.int32()),
            })
            _ingest(conn, table_name, tbl, "adbc.ingest.mode.create_append")
            assert _row_count(run_query, table_name) == 3
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_create_append_existing_table(self, conn, run_query, temp_table) -> None:
        run_query(f"INSERT INTO {temp_table} VALUES (1, 'pre', 0.0)")
        tbl = pa.table({
            "id": pa.array([2, 3], type=pa.int32()),
            "label": ["a", "b"],
            "value": [1.0, 2.0],
        })
        _ingest(conn, temp_table, tbl, "adbc.ingest.mode.create_append")
        assert _row_count(run_query, temp_table) == 3


# --------------------------------------------------------------------------- #
# Type coverage for create modes                                              #
# --------------------------------------------------------------------------- #

class TestCreateModeTypes:
    """The CREATE TABLE DDL is synthesised from the bound Arrow schema, so each
    Arrow type that maps to a Firebolt type needs to be exercised end-to-end."""

    def _ingest_and_drop(self, conn, run_query, table_name, tbl):
        try:
            _ingest(conn, table_name, tbl, "adbc.ingest.mode.create")
            assert _row_count(run_query, table_name) == tbl.num_rows
        finally:
            run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_int_and_string(self, conn, run_query, table_name) -> None:
        tbl = pa.table({
            "id": pa.array([1, 2], type=pa.int32()),
            "name": ["alpha", "beta"],
        })
        self._ingest_and_drop(conn, run_query, table_name, tbl)

    def test_int64_and_double(self, conn, run_query, table_name) -> None:
        tbl = pa.table({
            "id": pa.array([2**40, 2**41], type=pa.int64()),
            "v": pa.array([1.5, 2.5], type=pa.float64()),
        })
        self._ingest_and_drop(conn, run_query, table_name, tbl)

    def test_bool(self, conn, run_query, table_name) -> None:
        tbl = pa.table({
            "id": pa.array([1, 2], type=pa.int32()),
            "flag": pa.array([True, False], type=pa.bool_()),
        })
        self._ingest_and_drop(conn, run_query, table_name, tbl)

    def test_decimal(self, conn, run_query, table_name) -> None:
        tbl = pa.table({
            "id": pa.array([1, 2], type=pa.int32()),
            "amount": pa.array([Decimal("1.23"), Decimal("4.56")], type=pa.decimal128(10, 2)),
        })
        self._ingest_and_drop(conn, run_query, table_name, tbl)

    def test_array(self, conn, run_query, table_name) -> None:
        tbl = pa.table({
            "id": pa.array([1, 2], type=pa.int32()),
            "tags": pa.array([[1, 2, 3], [4]], type=pa.list_(pa.int32())),
        })
        self._ingest_and_drop(conn, run_query, table_name, tbl)
