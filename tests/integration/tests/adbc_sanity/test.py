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

"""ADBC sanity: connectivity, literal selects, arithmetic, multi-column / multi-row, string functions."""

import pyarrow as pa


class TestConnectivity:
    def test_connect(self, conn) -> None:
        """Driver loads and connection opens without error."""
        assert conn is not None

    def test_ping_via_query(self, run_query) -> None:
        """Simple SELECT 1 succeeds and returns exactly one row."""
        t = run_query("SELECT 1")
        assert t.num_rows == 1

    def test_result_is_arrow_table(self, run_query) -> None:
        """Result is a pyarrow.Table."""
        t = run_query("SELECT 1 AS x")
        assert isinstance(t, pa.Table)


class TestSelectLiterals:
    def test_integer_literal(self, run_query) -> None:
        t = run_query("SELECT 42 AS x")
        assert t["x"][0].as_py() == 42

    def test_negative_integer(self, run_query) -> None:
        t = run_query("SELECT -7 AS x")
        assert t["x"][0].as_py() == -7

    def test_float_literal(self, run_query) -> None:
        t = run_query("SELECT 3.14 AS x")
        assert abs(t["x"][0].as_py() - 3.14) < 1e-6

    def test_string_literal(self, run_query) -> None:
        t = run_query("SELECT 'hello' AS x")
        assert t["x"][0].as_py() == "hello"

    def test_empty_string(self, run_query) -> None:
        t = run_query("SELECT '' AS x")
        assert t["x"][0].as_py() == ""

    def test_boolean_true(self, run_query) -> None:
        t = run_query("SELECT true AS x")
        assert t["x"][0].as_py() is True

    def test_boolean_false(self, run_query) -> None:
        t = run_query("SELECT false AS x")
        assert t["x"][0].as_py() is False

    def test_null_literal(self, run_query) -> None:
        t = run_query("SELECT NULL AS x")
        assert t["x"][0].as_py() is None


class TestArithmetic:
    def test_addition(self, run_query) -> None:
        t = run_query("SELECT 2 + 3 AS x")
        assert t["x"][0].as_py() == 5

    def test_subtraction(self, run_query) -> None:
        t = run_query("SELECT 10 - 4 AS x")
        assert t["x"][0].as_py() == 6

    def test_multiplication(self, run_query) -> None:
        t = run_query("SELECT 6 * 7 AS x")
        assert t["x"][0].as_py() == 42

    def test_division(self, run_query) -> None:
        t = run_query("SELECT 10 / 4.0 AS x")
        assert abs(t["x"][0].as_py() - 2.5) < 1e-9

    def test_integer_division(self, run_query) -> None:
        t = run_query("SELECT 7 / 2 AS x")
        # Firebolt integer division truncates toward zero
        assert t["x"][0].as_py() == 3

    def test_modulo(self, run_query) -> None:
        t = run_query("SELECT 17 % 5 AS x")
        assert t["x"][0].as_py() == 2


class TestMultipleColumns:
    def test_two_columns(self, run_query) -> None:
        t = run_query("SELECT 1 AS a, 'x' AS b")
        assert t.num_columns == 2
        assert t["a"][0].as_py() == 1
        assert t["b"][0].as_py() == "x"

    def test_column_names_preserved(self, run_query) -> None:
        t = run_query("SELECT 1 AS foo, 2 AS bar, 3 AS baz")
        assert t.schema.names == ["foo", "bar", "baz"]

    def test_many_columns(self, run_query) -> None:
        cols = ", ".join(f"{i} AS c{i}" for i in range(10))
        t = run_query(f"SELECT {cols}")
        assert t.num_columns == 10
        for i in range(10):
            assert t[f"c{i}"][0].as_py() == i


class TestMultipleRows:
    def test_union_all_produces_multiple_rows(self, run_query) -> None:
        t = run_query(
            "SELECT 1 AS x UNION ALL "
            "SELECT 2 AS x UNION ALL "
            "SELECT 3 AS x"
        )
        assert t.num_rows == 3
        assert sorted(t["x"].to_pylist()) == [1, 2, 3]

    def test_row_count(self, run_query) -> None:
        n = 20
        selects = " UNION ALL ".join(f"SELECT {i} AS n" for i in range(n))
        t = run_query(selects)
        assert t.num_rows == n

    def test_values_in_order(self, run_query) -> None:
        t = run_query(
            "SELECT 'b' AS c UNION ALL "
            "SELECT 'c' AS c UNION ALL "
            "SELECT 'a' AS c "
            "ORDER BY c"
        )
        assert t["c"].to_pylist() == ["a", "b", "c"]


class TestStringFunctions:
    def test_upper(self, run_query) -> None:
        t = run_query("SELECT upper('hello') AS x")
        assert t["x"][0].as_py() == "HELLO"

    def test_lower(self, run_query) -> None:
        t = run_query("SELECT lower('WORLD') AS x")
        assert t["x"][0].as_py() == "world"

    def test_length(self, run_query) -> None:
        t = run_query("SELECT length('firebolt') AS x")
        assert t["x"][0].as_py() == 8

    def test_concat(self, run_query) -> None:
        t = run_query("SELECT concat('foo', 'bar') AS x")
        assert t["x"][0].as_py() == "foobar"

    def test_trim(self, run_query) -> None:
        t = run_query("SELECT trim('  hello  ') AS x")
        assert t["x"][0].as_py() == "hello"
