"""
Advanced SQL query tests.

Covers JOINs, CTEs (WITH), CASE expressions, HAVING, subqueries, and
window functions — verifying that the driver correctly handles multi-step
queries that return non-trivial result shapes.
"""

import pyarrow as pa
import pytest


class TestJoins:
    def test_inner_join(self, run_query, table_name) -> None:
        left = table_name + "_l"
        right = table_name + "_r"
        run_query(f"CREATE TABLE {left} (id INT, val TEXT)")
        run_query(f"CREATE TABLE {right} (id INT, extra TEXT)")
        run_query(f"INSERT INTO {left} VALUES (1, 'a'), (2, 'b'), (3, 'c')")
        run_query(f"INSERT INTO {right} VALUES (1, 'x'), (3, 'z')")
        t = run_query(
            f"SELECT {left}.id, {left}.val, {right}.extra "
            f"FROM {left} JOIN {right} ON {left}.id = {right}.id "
            f"ORDER BY {left}.id"
        )
        assert t.num_rows == 2
        assert t["id"].to_pylist() == [1, 3]
        assert t["val"].to_pylist() == ["a", "c"]
        assert t["extra"].to_pylist() == ["x", "z"]
        run_query(f"DROP TABLE {left}")
        run_query(f"DROP TABLE {right}")

    def test_left_join_includes_nulls(self, run_query, table_name) -> None:
        left = table_name + "_l"
        right = table_name + "_r"
        run_query(f"CREATE TABLE {left} (id INT)")
        run_query(f"CREATE TABLE {right} (id INT, val TEXT)")
        run_query(f"INSERT INTO {left} VALUES (1), (2), (3)")
        run_query(f"INSERT INTO {right} VALUES (1, 'one'), (3, 'three')")
        t = run_query(
            f"SELECT {left}.id, {right}.val "
            f"FROM {left} LEFT JOIN {right} ON {left}.id = {right}.id "
            f"ORDER BY {left}.id"
        )
        assert t.num_rows == 3
        assert t["val"][1].as_py() is None  # id=2 has no match
        run_query(f"DROP TABLE {left}")
        run_query(f"DROP TABLE {right}")

    def test_cross_join(self, run_query) -> None:
        t = run_query(
            "SELECT a.x, b.y "
            "FROM (SELECT 1 AS x UNION ALL SELECT 2 AS x) a "
            "CROSS JOIN (SELECT 'p' AS y UNION ALL SELECT 'q' AS y) b "
            "ORDER BY a.x, b.y"
        )
        assert t.num_rows == 4

    def test_self_join(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (id INT, parent_id INT)")
        run_query(
            f"INSERT INTO {table_name} VALUES (1, NULL), (2, 1), (3, 1), (4, 2)"
        )
        t = run_query(
            f"SELECT c.id AS child, p.id AS parent "
            f"FROM {table_name} c JOIN {table_name} p ON c.parent_id = p.id "
            f"ORDER BY c.id"
        )
        assert t.num_rows == 3
        assert t["child"].to_pylist() == [2, 3, 4]
        run_query(f"DROP TABLE {table_name}")


class TestCTE:
    def test_simple_cte(self, run_query) -> None:
        t = run_query(
            "WITH nums AS (SELECT 1 AS n UNION ALL SELECT 2 UNION ALL SELECT 3) "
            "SELECT SUM(n) AS total FROM nums"
        )
        assert t["total"][0].as_py() == 6

    def test_cte_with_filter(self, run_query, temp_table) -> None:
        for i in range(10):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'r{i}', {i}.0)")
        t = run_query(
            f"WITH big AS (SELECT id FROM {temp_table} WHERE id >= 7) "
            f"SELECT COUNT(*) AS n FROM big"
        )
        assert t["n"][0].as_py() == 3

    def test_chained_cte(self, run_query) -> None:
        t = run_query(
            "WITH "
            "  base AS (SELECT 10 AS x UNION ALL SELECT 20 UNION ALL SELECT 30), "
            "  doubled AS (SELECT x * 2 AS y FROM base) "
            "SELECT SUM(y) AS total FROM doubled"
        )
        assert t["total"][0].as_py() == 120

    def test_cte_used_twice_in_join(self, run_query) -> None:
        t = run_query(
            "WITH nums AS (SELECT 1 AS n UNION ALL SELECT 2 UNION ALL SELECT 3) "
            "SELECT a.n AS a, b.n AS b FROM nums a JOIN nums b ON a.n = b.n "
            "ORDER BY a.n"
        )
        assert t.num_rows == 3
        assert t["a"].to_pylist() == t["b"].to_pylist()


class TestCaseExpression:
    def test_simple_case(self, run_query) -> None:
        t = run_query(
            "SELECT CASE 2 WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'other' END AS x"
        )
        assert t["x"][0].as_py() == "two"

    def test_searched_case(self, run_query) -> None:
        t = run_query(
            "SELECT CASE WHEN 5 > 3 THEN 'yes' ELSE 'no' END AS x"
        )
        assert t["x"][0].as_py() == "yes"

    def test_case_with_null(self, run_query) -> None:
        t = run_query(
            "SELECT CASE WHEN NULL THEN 'yes' ELSE 'no' END AS x"
        )
        assert t["x"][0].as_py() == "no"

    def test_case_in_aggregate(self, run_query, temp_table) -> None:
        for i in range(6):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'x', 0.0)")
        t = run_query(
            f"SELECT "
            f"  SUM(CASE WHEN id % 2 = 0 THEN 1 ELSE 0 END) AS evens, "
            f"  SUM(CASE WHEN id % 2 != 0 THEN 1 ELSE 0 END) AS odds "
            f"FROM {temp_table}"
        )
        assert t["evens"][0].as_py() == 3
        assert t["odds"][0].as_py() == 3

    def test_nested_case(self, run_query) -> None:
        t = run_query(
            "SELECT CASE "
            "  WHEN 10 > 5 THEN CASE WHEN 3 > 1 THEN 'deep' ELSE 'mid' END "
            "  ELSE 'top' "
            "END AS x"
        )
        assert t["x"][0].as_py() == "deep"


class TestHaving:
    def test_having_filters_groups(self, run_query, temp_table) -> None:
        # ids 0..9 — groups by id%3; only keep groups with count >= 3
        for i in range(9):
            run_query(f"INSERT INTO {temp_table} VALUES ({i % 3}, 'x', 0.0)")
        t = run_query(
            f"SELECT id, COUNT(*) AS n "
            f"FROM {temp_table} "
            f"GROUP BY id "
            f"HAVING COUNT(*) >= 3 "
            f"ORDER BY id"
        )
        assert t.num_rows == 3
        assert all(n == 3 for n in t["n"].to_pylist())

    def test_having_with_sum(self, run_query, temp_table) -> None:
        for i in range(1, 6):
            run_query(f"INSERT INTO {temp_table} VALUES ({i % 2}, 'x', {i}.0)")
        t = run_query(
            f"SELECT id, SUM(value) AS s "
            f"FROM {temp_table} "
            f"GROUP BY id "
            f"HAVING SUM(value) > 5 "
            f"ORDER BY id"
        )
        assert t.num_rows >= 1
        assert all(s > 5 for s in t["s"].to_pylist())


class TestSubqueries:
    def test_scalar_subquery(self, run_query, temp_table) -> None:
        for i in range(5):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'x', {i}.0)")
        t = run_query(
            f"SELECT id FROM {temp_table} "
            f"WHERE value = (SELECT MAX(value) FROM {temp_table})"
        )
        assert t.num_rows == 1
        assert t["id"][0].as_py() == 4

    def test_in_subquery(self, run_query, table_name) -> None:
        left = table_name + "_l"
        right = table_name + "_r"
        run_query(f"CREATE TABLE {left} (id INT)")
        run_query(f"CREATE TABLE {right} (id INT)")
        run_query(f"INSERT INTO {left} VALUES (1), (2), (3), (4)")
        run_query(f"INSERT INTO {right} VALUES (2), (4)")
        t = run_query(
            f"SELECT id FROM {left} WHERE id IN (SELECT id FROM {right}) ORDER BY id"
        )
        assert t["id"].to_pylist() == [2, 4]
        run_query(f"DROP TABLE {left}")
        run_query(f"DROP TABLE {right}")

    def test_derived_table(self, run_query) -> None:
        t = run_query(
            "SELECT sub.x * 2 AS y "
            "FROM (SELECT 21 AS x) sub"
        )
        assert t["y"][0].as_py() == 42

    def test_exists_subquery(self, run_query, table_name) -> None:
        outer = table_name + "_o"
        inner = table_name + "_i"
        run_query(f"CREATE TABLE {outer} (id INT)")
        run_query(f"CREATE TABLE {inner} (id INT)")
        run_query(f"INSERT INTO {outer} VALUES (1), (2), (3)")
        run_query(f"INSERT INTO {inner} VALUES (2)")
        t = run_query(
            f"SELECT id FROM {outer} "
            f"WHERE EXISTS (SELECT 1 FROM {inner} WHERE {inner}.id = {outer}.id) "
            f"ORDER BY id"
        )
        assert t["id"].to_pylist() == [2]
        run_query(f"DROP TABLE {outer}")
        run_query(f"DROP TABLE {inner}")


class TestWindowFunctions:
    def test_row_number(self, run_query, temp_table) -> None:
        for i in range(5):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'x', 0.0)")
        t = run_query(
            f"SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn "
            f"FROM {temp_table} "
            f"ORDER BY id"
        )
        assert t["rn"].to_pylist() == [1, 2, 3, 4, 5]

    def test_rank(self, run_query, temp_table) -> None:
        # Two rows with id=1 should both get rank 1; id=2 gets rank 3.
        for v in [1, 1, 2]:
            run_query(f"INSERT INTO {temp_table} VALUES ({v}, 'x', 0.0)")
        t = run_query(
            f"SELECT id, RANK() OVER (ORDER BY id) AS rnk "
            f"FROM {temp_table} "
            f"ORDER BY id, rnk"
        )
        ranks = t["rnk"].to_pylist()
        assert ranks[0] == 1
        assert ranks[1] == 1
        assert ranks[2] == 3

    def test_sum_over_partition(self, run_query, temp_table) -> None:
        # group 0: ids 0,2,4 (value sum=6); group 1: ids 1,3 (value sum=4)
        for i in range(5):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'g{i % 2}', {i}.0)")
        t = run_query(
            f"SELECT label, SUM(value) OVER (PARTITION BY label) AS s "
            f"FROM {temp_table} "
            f"ORDER BY id"
        )
        # Verify partition sums are consistent within each label group.
        rows = list(zip(t["label"].to_pylist(), t["s"].to_pylist()))
        sums_by_label = {}
        for label, s in rows:
            if label in sums_by_label:
                assert sums_by_label[label] == s
            else:
                sums_by_label[label] = s
        assert abs(sums_by_label["g0"] - 6.0) < 1e-9
        assert abs(sums_by_label["g1"] - 4.0) < 1e-9

    def test_lag(self, run_query, temp_table) -> None:
        for i in range(4):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'x', 0.0)")
        t = run_query(
            f"SELECT id, LAG(id) OVER (ORDER BY id) AS prev "
            f"FROM {temp_table} "
            f"ORDER BY id"
        )
        prev = t["prev"].to_pylist()
        assert prev[0] is None
        assert prev[1] == 0
        assert prev[2] == 1
        assert prev[3] == 2

    def test_lead(self, run_query, temp_table) -> None:
        for i in range(4):
            run_query(f"INSERT INTO {temp_table} VALUES ({i}, 'x', 0.0)")
        t = run_query(
            f"SELECT id, LEAD(id) OVER (ORDER BY id) AS nxt "
            f"FROM {temp_table} "
            f"ORDER BY id"
        )
        nxt = t["nxt"].to_pylist()
        assert nxt[0] == 1
        assert nxt[-1] is None
