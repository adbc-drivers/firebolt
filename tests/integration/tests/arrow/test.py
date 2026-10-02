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
Arrow-specific tests.

Verify that the driver produces well-formed Arrow data: correct schema types,
correct handling of large result sets (multi-batch), and that the Arrow C Stream
interface is properly implemented.
"""

import pyarrow as pa
import pytest


class TestSchema:
    def test_schema_has_correct_field_names(self, run_query) -> None:
        t = run_query("SELECT 1 AS alpha, 2 AS beta, 3 AS gamma")
        assert t.schema.names == ["alpha", "beta", "gamma"]

    def test_integer_column_type(self, run_query) -> None:
        t = run_query("SELECT 1::INT AS x")
        assert pa.types.is_integer(t.schema.field("x").type)

    def test_bigint_column_type(self, run_query) -> None:
        t = run_query("SELECT 1::BIGINT AS x")
        assert pa.types.is_integer(t.schema.field("x").type)

    def test_float_column_type(self, run_query) -> None:
        t = run_query("SELECT 1.0::FLOAT AS x")
        assert pa.types.is_floating(t.schema.field("x").type)

    def test_double_column_type(self, run_query) -> None:
        t = run_query("SELECT 1.0::DOUBLE AS x")
        assert pa.types.is_floating(t.schema.field("x").type)

    def test_text_column_type(self, run_query) -> None:
        t = run_query("SELECT 'x'::TEXT AS x")
        assert pa.types.is_string(t.schema.field("x").type) or pa.types.is_large_string(
            t.schema.field("x").type
        )

    def test_boolean_column_type(self, run_query) -> None:
        t = run_query("SELECT true AS x")
        assert pa.types.is_boolean(t.schema.field("x").type)

    def test_null_column_has_null_count(self, run_query) -> None:
        t = run_query("SELECT NULL::INT AS x")
        assert t["x"].null_count == 1

    def test_non_null_column_has_zero_null_count(self, run_query) -> None:
        t = run_query("SELECT 42 AS x")
        assert t["x"].null_count == 0

    def test_mixed_null_column_null_count(self, run_query) -> None:
        t = run_query(
            "SELECT 1 AS x UNION ALL SELECT NULL AS x UNION ALL SELECT 3 AS x"
        )
        assert t["x"].null_count == 1

    def test_schema_nullable_flag(self, run_query) -> None:
        t = run_query("SELECT NULL::INT AS x")
        # Nullable columns should have nullable=True in Arrow schema
        assert t.schema.field("x").nullable is True


class TestResultShape:
    def test_empty_result(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (id INT)")
        t = run_query(f"SELECT id FROM {table_name}")
        assert t.num_rows == 0
        assert t.num_columns == 1
        run_query(f"DROP TABLE {table_name}")

    def test_single_row_single_col(self, run_query) -> None:
        t = run_query("SELECT 99 AS x")
        assert t.num_rows == 1
        assert t.num_columns == 1

    def test_many_rows_preserved(self, run_query, temp_table) -> None:
        n = 1000
        # Insert rows in batches using UNION ALL to reduce round-trips.
        batch_size = 100
        for start in range(0, n, batch_size):
            values = ", ".join(
                f"({i}, 'label_{i}', {i}.0)"
                for i in range(start, min(start + batch_size, n))
            )
            run_query(f"INSERT INTO {temp_table} VALUES {values}")
        t = run_query(f"SELECT id FROM {temp_table}")
        assert t.num_rows == n

    def test_wide_result(self, run_query) -> None:
        cols = ", ".join(f"{i}::INT AS c{i}" for i in range(50))
        t = run_query(f"SELECT {cols}")
        assert t.num_columns == 50
        assert t.num_rows == 1

    def test_result_values_correct_for_large_dataset(self, run_query, temp_table) -> None:
        n = 500
        values = ", ".join(f"({i}, 'x', 0.0)" for i in range(n))
        run_query(f"INSERT INTO {temp_table} VALUES {values}")
        t = run_query(f"SELECT id FROM {temp_table} ORDER BY id")
        assert t["id"].to_pylist() == list(range(n))


class TestArrowCStreamInterface:
    """Test that the ArrowArrayStream C interface is correctly implemented."""

    def test_stream_can_be_read_multiple_times(self, run_query) -> None:
        # Each call to run_query creates a fresh stream; verify two consecutive
        # queries each return correct results.
        t1 = run_query("SELECT 1 AS x")
        t2 = run_query("SELECT 2 AS x")
        assert t1["x"][0].as_py() == 1
        assert t2["x"][0].as_py() == 2

    def test_concurrent_statements(self, conn) -> None:
        """Two statements opened on the same connection run sequentially."""
        import adbc_driver_manager

        results = []
        for val in [10, 20]:
            with adbc_driver_manager.AdbcStatement(conn) as stmt:
                stmt.set_sql_query(f"SELECT {val} AS x")
                reader, _ = stmt.execute_query()
                results.append(pa.RecordBatchReader.from_stream(reader).read_all()["x"][0].as_py())

        assert results == [10, 20]

    def test_pyarrow_table_to_pandas(self, run_query) -> None:
        """Arrow table can be converted to pandas (validates Arrow correctness)."""
        pytest.importorskip("pandas")
        t = run_query("SELECT 1 AS a, 'hello' AS b, 3.14 AS c")
        df = t.to_pandas()
        assert df.shape == (1, 3)
        assert df["a"].iloc[0] == 1
        assert df["b"].iloc[0] == "hello"

    def test_chunked_array_values(self, run_query) -> None:
        """Individual array chunks all contain valid data."""
        t = run_query(
            " UNION ALL ".join(f"SELECT {i} AS x" for i in range(20))
        )
        values = []
        for chunk in t["x"].chunks:
            values.extend(chunk.to_pylist())
        assert sorted(values) == list(range(20))


class TestErrorHandling:
    def test_syntax_error_raises(self, conn) -> None:
        import adbc_driver_manager

        with pytest.raises(Exception):
            with adbc_driver_manager.AdbcStatement(conn) as stmt:
                stmt.set_sql_query("THIS IS NOT VALID SQL !@#$")
                stmt.execute_query()

    def test_unknown_table_raises(self, conn) -> None:
        import adbc_driver_manager

        with pytest.raises(Exception):
            with adbc_driver_manager.AdbcStatement(conn) as stmt:
                stmt.set_sql_query("SELECT * FROM table_that_does_not_exist_xyz")
                stmt.execute_query()

    def test_type_error_raises(self, conn) -> None:
        import adbc_driver_manager

        with pytest.raises(Exception):
            with adbc_driver_manager.AdbcStatement(conn) as stmt:
                stmt.set_sql_query("SELECT 'abc' + 123")
                stmt.execute_query()

    def test_division_by_zero(self, run_query) -> None:
        # Firebolt may return NULL or raise; just verify it doesn't crash the driver.
        try:
            t = run_query("SELECT 1 / 0 AS x")
            # If it returns a result, value should be NULL or infinity
            v = t["x"][0].as_py()
            assert v is None or v != v or abs(v) > 1e30  # NULL, NaN, or inf
        except Exception:
            pass  # Raising is also acceptable

    def test_connection_remains_usable_after_error(self, conn) -> None:
        """A query error must not invalidate the connection."""
        import adbc_driver_manager

        # Fire a bad query.
        try:
            with adbc_driver_manager.AdbcStatement(conn) as stmt:
                stmt.set_sql_query("SELECT * FROM no_such_table_zzz")
                stmt.execute_query()
        except Exception:
            pass

        # Connection should still work.
        with adbc_driver_manager.AdbcStatement(conn) as stmt:
            stmt.set_sql_query("SELECT 42 AS x")
            reader, _ = stmt.execute_query()
            assert pa.RecordBatchReader.from_stream(reader).read_all()["x"][0].as_py() == 42
