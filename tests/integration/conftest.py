"""Pytest fixtures shared across the firebolt-adbc integration suite."""

import os
import os.path as p
import uuid

import adbc_driver_manager
import pyarrow as pa
import pytest

from helpers import firebolt_core


ADBC_DRIVER_PATH = os.environ.get(
    "PACKDB_TESTS_ADBC_BINARY_PATH",
    p.normpath(p.join(p.dirname(__file__), "..", "..", "build", "libfirebolt_adbc.so")),
)


@pytest.fixture(scope="session")
def started_core():
    product = firebolt_core.FireboltCore(__file__)
    product.add_engine(engine_name="engine1", num_nodes=1)
    product.start()
    try:
        yield product
    finally:
        product.stop()


@pytest.fixture(scope="session")
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


@pytest.fixture
def table_name() -> str:
    """A unique table name for the duration of one test."""
    return f"adbc_test_{uuid.uuid4().hex[:8]}"


@pytest.fixture
def temp_table(run_query, table_name: str):
    """Create a (id INT, label TEXT, value DOUBLE) table; drop on teardown."""
    run_query(
        f"""
        CREATE TABLE {table_name} (
            id     INT,
            label  TEXT,
            value  DOUBLE
        )
        """
    )
    yield table_name
    run_query(f"DROP TABLE IF EXISTS {table_name}")
