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
ARRAY type tests.

Verify that Firebolt ARRAY columns are correctly represented in Arrow as
list<T> arrays, that element values and nulls are faithfully round-tripped,
and that nested arrays work correctly.
"""

import pyarrow as pa


class TestArrayLiterals:
    def test_int_array_literal(self, run_query) -> None:
        t = run_query("SELECT [1, 2, 3] AS x")
        v = t["x"][0].as_py()
        assert v == [1, 2, 3]

    def test_text_array_literal(self, run_query) -> None:
        t = run_query("SELECT ['a', 'b', 'c'] AS x")
        assert t["x"][0].as_py() == ["a", "b", "c"]

    def test_empty_array(self, run_query) -> None:
        t = run_query("SELECT []::ARRAY(INT) AS x")
        assert t["x"][0].as_py() == []

    def test_single_element_array(self, run_query) -> None:
        t = run_query("SELECT [42] AS x")
        assert t["x"][0].as_py() == [42]

    def test_null_array(self, run_query) -> None:
        t = run_query("SELECT NULL::ARRAY(INT) AS x")
        assert t["x"][0].as_py() is None

    def test_arrow_type_is_list(self, run_query) -> None:
        t = run_query("SELECT [1, 2, 3] AS x")
        assert pa.types.is_list(t.schema.field("x").type)

    def test_list_element_type_is_integer(self, run_query) -> None:
        t = run_query("SELECT [1::INT, 2::INT] AS x")
        field_type = t.schema.field("x").type
        assert pa.types.is_list(field_type)
        assert pa.types.is_integer(field_type.value_type)


class TestArrayRoundtrip:
    def test_int_array_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (vals ARRAY(INT))")
        run_query(f"INSERT INTO {table_name} VALUES ([10, 20, 30])")
        t = run_query(f"SELECT vals FROM {table_name}")
        assert t["vals"][0].as_py() == [10, 20, 30]
        run_query(f"DROP TABLE {table_name}")

    def test_text_array_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (tags ARRAY(TEXT))")
        run_query(f"INSERT INTO {table_name} VALUES (['foo', 'bar', 'baz'])")
        t = run_query(f"SELECT tags FROM {table_name}")
        assert t["tags"][0].as_py() == ["foo", "bar", "baz"]
        run_query(f"DROP TABLE {table_name}")

    def test_double_array_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (scores ARRAY(DOUBLE))")
        run_query(f"INSERT INTO {table_name} VALUES ([1.1, 2.2, 3.3])")
        t = run_query(f"SELECT scores FROM {table_name}")
        vals = t["scores"][0].as_py()
        assert len(vals) == 3
        assert abs(vals[0] - 1.1) < 1e-9
        assert abs(vals[1] - 2.2) < 1e-9
        assert abs(vals[2] - 3.3) < 1e-9
        run_query(f"DROP TABLE {table_name}")

    def test_multiple_rows_with_arrays(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (id INT, vals ARRAY(INT))")
        run_query(
            f"INSERT INTO {table_name} VALUES (1, [1, 2]), (2, [3, 4, 5]), (3, [])"
        )
        t = run_query(f"SELECT id, vals FROM {table_name} ORDER BY id")
        assert t["vals"][0].as_py() == [1, 2]
        assert t["vals"][1].as_py() == [3, 4, 5]
        assert t["vals"][2].as_py() == []
        run_query(f"DROP TABLE {table_name}")

    def test_null_element_in_array(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (vals ARRAY(INT))")
        run_query(f"INSERT INTO {table_name} VALUES ([1, NULL, 3])")
        t = run_query(f"SELECT vals FROM {table_name}")
        vals = t["vals"][0].as_py()
        assert vals[0] == 1
        assert vals[1] is None
        assert vals[2] == 3
        run_query(f"DROP TABLE {table_name}")

    def test_null_array_row(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (vals ARRAY(INT))")
        run_query(f"INSERT INTO {table_name} VALUES ([1, 2]), (NULL), ([3])")
        t = run_query(f"SELECT vals FROM {table_name} ORDER BY vals[1]")
        values = [t["vals"][i].as_py() for i in range(3)]
        assert None in values
        assert [1, 2] in values
        assert [3] in values
        run_query(f"DROP TABLE {table_name}")


class TestArrayFunctions:
    def test_array_length(self, run_query) -> None:
        t = run_query("SELECT array_length([10, 20, 30]) AS n")
        assert t["n"][0].as_py() == 3

    def test_array_element_access(self, run_query) -> None:
        # Firebolt arrays are 1-indexed.
        t = run_query("SELECT [10, 20, 30][2] AS x")
        assert t["x"][0].as_py() == 20

    def test_array_contains(self, run_query) -> None:
        t = run_query("SELECT array_contains([1, 2, 3], 2) AS x")
        assert t["x"][0].as_py() is True

    def test_array_contains_missing(self, run_query) -> None:
        t = run_query("SELECT array_contains([1, 2, 3], 9) AS x")
        assert t["x"][0].as_py() is False

    def test_array_sort(self, run_query) -> None:
        t = run_query("SELECT array_sort([3, 1, 2]) AS x")
        assert t["x"][0].as_py() == [1, 2, 3]


class TestNestedArrays:
    def test_array_of_arrays_literal(self, run_query) -> None:
        t = run_query("SELECT [[1, 2], [3, 4]] AS x")
        v = t["x"][0].as_py()
        assert v == [[1, 2], [3, 4]]

    def test_nested_array_arrow_type(self, run_query) -> None:
        t = run_query("SELECT [[1, 2], [3]] AS x")
        field_type = t.schema.field("x").type
        assert pa.types.is_list(field_type)
        assert pa.types.is_list(field_type.value_type)

    def test_nested_array_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (matrix ARRAY(ARRAY(INT)))")
        run_query(f"INSERT INTO {table_name} VALUES ([[1, 2], [3, 4, 5]])")
        t = run_query(f"SELECT matrix FROM {table_name}")
        assert t["matrix"][0].as_py() == [[1, 2], [3, 4, 5]]
        run_query(f"DROP TABLE {table_name}")
