"""ADBC integration tests against a 1-node Firebolt Core."""

import os
import os.path as p

import adbc_driver_manager
import pyarrow as pa
import pytest

from helpers import firebolt_core


ADBC_DRIVER_PATH = os.environ.get(
    "PACKDB_TESTS_ADBC_BINARY_PATH",
    p.normpath(p.join(p.dirname(__file__), "..", "..", "..", "..", "build", "libfirebolt_adbc.so")),
)


@pytest.fixture(scope="module")
def started_core():
    product = firebolt_core.FireboltCore(__file__)
    product.add_engine(engine_name="engine1", num_nodes=1)
    product.start()
    try:
        yield product
    finally:
        product.stop()


@pytest.fixture(scope="module")
def server_url(started_core):
    assert os.path.isfile(ADBC_DRIVER_PATH), f"ADBC driver not found at {ADBC_DRIVER_PATH}"
    node = started_core.engines["engine1"].instances["node_1"]
    return f"http://{node.pg_host}:{firebolt_core.FIREBOLT_CORE_QUERY_ENDPOINT}"


@pytest.fixture
def conn(server_url):
    """A fresh AdbcConnection for each test."""
    with adbc_driver_manager.AdbcDatabase(driver=ADBC_DRIVER_PATH, uri=server_url) as db:
        with adbc_driver_manager.AdbcConnection(db) as connection:
            yield connection


@pytest.fixture
def run_query(conn):
    """Return a callable that runs SQL and returns a pyarrow.Table."""

    def _run(sql: str) -> pa.Table:
        with adbc_driver_manager.AdbcStatement(conn) as stmt:
            stmt.set_sql_query(sql)
            reader, _ = stmt.execute_query()
            return pa.RecordBatchReader.from_stream(reader).read_all()

    return _run


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
            "SELECT 'a' AS c UNION ALL "
            "SELECT 'b' AS c UNION ALL "
            "SELECT 'c' AS c "
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
