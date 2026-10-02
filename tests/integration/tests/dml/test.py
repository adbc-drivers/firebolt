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
DDL and DML tests.

Tests CREATE TABLE, INSERT, SELECT, and DROP TABLE, plus aggregate functions
and filtering.  Each test uses an isolated table via the temp_table fixture
(which drops the table on teardown) so tests can run in any order.
"""

import pyarrow as pa
import pytest


class TestDDL:
    def test_create_and_drop_table(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (id INT, name TEXT)")
        run_query(f"DROP TABLE {table_name}")

    def test_create_if_not_exists(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (id INT)")
        # Second CREATE IF NOT EXISTS must not raise.
        run_query(f"CREATE TABLE IF NOT EXISTS {table_name} (id INT)")
        run_query(f"DROP TABLE {table_name}")

    def test_drop_if_exists(self, run_query, table_name) -> None:
        # DROP TABLE IF EXISTS on a non-existent table must not raise.
        run_query(f"DROP TABLE IF EXISTS {table_name}")

    def test_ddl_returns_empty_table(self, run_query, table_name) -> None:
        t = run_query(f"CREATE TABLE {table_name} (id INT)")
        assert isinstance(t, pa.Table)
        assert t.num_rows == 0
        run_query(f"DROP TABLE {table_name}")


class TestInsertSelect:
    def test_insert_single_row(self, run_query, temp_table) -> None:
        run_query(f"INSERT INTO {temp_table} VALUES (1, 'alpha', 1.1)")
        t = run_query(f"SELECT id, label, value FROM {temp_table}")
        assert t.num_rows == 1
        assert t["id"][0].as_py() == 1
        assert t["label"][0].as_py() == "alpha"
        assert abs(t["value"][0].as_py() - 1.1) < 1e-9

    def test_insert_multiple_rows(self, run_query, temp_table) -> None:
        for i in range(5):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'row{i}', {i * 0.5})")
        t = run_query(f"SELECT COUNT(*) AS n FROM {temp_table}")
        assert t["n"][0].as_py() == 5

    def test_insert_null_values(self, run_query, temp_table) -> None:
        run_query(f"INSERT INTO {temp_table} VALUES (1, NULL, NULL)")
        t = run_query(f"SELECT label, value FROM {temp_table}")
        assert t["label"][0].as_py() is None
        assert t["value"][0].as_py() is None

    def test_select_where_clause(self, run_query, temp_table) -> None:
        for i in range(10):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'r{i}', {i}.0)")
        t = run_query(f"SELECT id FROM {temp_table} WHERE id > 6 ORDER BY id")
        assert t["id"].to_pylist() == [7, 8, 9]

    def test_select_with_limit(self, run_query, temp_table) -> None:
        for i in range(10):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'r{i}', {i}.0)")
        t = run_query(f"SELECT id FROM {temp_table} ORDER BY id LIMIT 3")
        assert t.num_rows == 3
        assert t["id"].to_pylist() == [0, 1, 2]

    def test_select_order_by(self, run_query, temp_table) -> None:
        for v in [3, 1, 4, 1, 5, 9, 2, 6]:
            run_query(f"INSERT INTO {temp_table} VALUES ({v}, 'x', 0.0)")
        t = run_query(f"SELECT id FROM {temp_table} ORDER BY id")
        assert t["id"].to_pylist() == sorted([3, 1, 4, 1, 5, 9, 2, 6])

    def test_select_order_by_desc(self, run_query, temp_table) -> None:
        for v in [1, 2, 3]:
            run_query(f"INSERT INTO {temp_table} VALUES ({v}, 'x', 0.0)")
        t = run_query(f"SELECT id FROM {temp_table} ORDER BY id DESC")
        assert t["id"].to_pylist() == [3, 2, 1]


class TestAggregates:
    def _populate(self, run_query, name: str) -> None:
        rows = [(1, 10.0), (2, 20.0), (3, 30.0), (4, 40.0), (5, 50.0)]
        for id_, val in rows:
            run_query(f"INSERT INTO {name} VALUES ({id_}, 'r{id_}', {val})")

    def test_count_star(self, run_query, temp_table) -> None:
        self._populate(run_query, temp_table)
        t = run_query(f"SELECT COUNT(*) AS n FROM {temp_table}")
        assert t["n"][0].as_py() == 5

    def test_sum(self, run_query, temp_table) -> None:
        self._populate(run_query, temp_table)
        t = run_query(f"SELECT SUM(value) AS s FROM {temp_table}")
        assert abs(t["s"][0].as_py() - 150.0) < 1e-9

    def test_avg(self, run_query, temp_table) -> None:
        self._populate(run_query, temp_table)
        t = run_query(f"SELECT AVG(value) AS a FROM {temp_table}")
        assert abs(t["a"][0].as_py() - 30.0) < 1e-9

    def test_min(self, run_query, temp_table) -> None:
        self._populate(run_query, temp_table)
        t = run_query(f"SELECT MIN(value) AS m FROM {temp_table}")
        assert abs(t["m"][0].as_py() - 10.0) < 1e-9

    def test_max(self, run_query, temp_table) -> None:
        self._populate(run_query, temp_table)
        t = run_query(f"SELECT MAX(value) AS m FROM {temp_table}")
        assert abs(t["m"][0].as_py() - 50.0) < 1e-9

    def test_count_distinct(self, run_query, temp_table) -> None:
        for v in [1, 1, 2, 2, 3]:
            run_query(f"INSERT INTO {temp_table} VALUES ({v}, 'x', 0.0)")
        t = run_query(f"SELECT COUNT(DISTINCT id) AS n FROM {temp_table}")
        assert t["n"][0].as_py() == 3

    def test_group_by(self, run_query, temp_table) -> None:
        # Insert rows with two distinct labels.
        for i in range(3):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'even', {i}.0)")
        for i in range(3, 6):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'odd', {i}.0)")
        t = run_query(
            f"SELECT label, COUNT(*) AS n "
            f"FROM {temp_table} "
            f"GROUP BY label "
            f"ORDER BY label"
        )
        assert t.num_rows == 2
        counts = dict(zip(t["label"].to_pylist(), t["n"].to_pylist()))
        assert counts["even"] == 3
        assert counts["odd"] == 3

    def test_count_empty_table(self, run_query, temp_table) -> None:
        t = run_query(f"SELECT COUNT(*) AS n FROM {temp_table}")
        assert t["n"][0].as_py() == 0


class TestDataTypes:
    """Type-round-trip tests through a real table (not just a literal SELECT)."""

    def test_int_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (x INT)")
        run_query(f"INSERT INTO {table_name} VALUES (123)")
        t = run_query(f"SELECT x FROM {table_name}")
        assert t["x"][0].as_py() == 123
        run_query(f"DROP TABLE {table_name}")

    def test_text_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (x TEXT)")
        run_query(f"INSERT INTO {table_name} VALUES ('hello')")
        t = run_query(f"SELECT x FROM {table_name}")
        assert t["x"][0].as_py() == "hello"
        run_query(f"DROP TABLE {table_name}")

    def test_double_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (x DOUBLE)")
        run_query(f"INSERT INTO {table_name} VALUES (2.718281828)")
        t = run_query(f"SELECT x FROM {table_name}")
        assert abs(t["x"][0].as_py() - 2.718281828) < 1e-6
        run_query(f"DROP TABLE {table_name}")

    def test_boolean_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (x BOOLEAN)")
        run_query(f"INSERT INTO {table_name} VALUES (true)")
        t = run_query(f"SELECT x FROM {table_name}")
        assert t["x"][0].as_py() is True
        run_query(f"DROP TABLE {table_name}")

    def test_null_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (x INT)")
        run_query(f"INSERT INTO {table_name} VALUES (NULL)")
        t = run_query(f"SELECT x FROM {table_name}")
        assert t["x"][0].as_py() is None
        run_query(f"DROP TABLE {table_name}")
