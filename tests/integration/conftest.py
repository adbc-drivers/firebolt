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

"""Pytest fixtures shared across the firebolt-adbc integration suite."""

import os
import os.path as p
import uuid

import adbc_driver_manager
import pyarrow as pa
import pytest
from adbc_driver_manager import dbapi
from helpers import firebolt_engine
from helpers.mock_firebolt_server import MockFireboltServer

ADBC_DRIVER_PATH = os.environ.get(
    "PACKDB_TESTS_ADBC_BINARY_PATH",
    p.normpath(
        p.join(p.dirname(__file__), "..", "..", "build", "libadbc_driver_firebolt.so")
    ),
)

# What the engine fixtures (conn, dbapi_conn, cursor, run_query, …) connect to:
#   core (default)  the 1-node local engine this suite starts in Docker
#   fb2             FB2 only: an engine in a Firebolt 2.0 SaaS account, through FB2
#                   SaaS (Legacy) mode.  Account, engine and credentials come from
#                   FIREBOLT_FB2_* (see fb2_db_kwargs) and are never committed;
#                   runner.py forwards them by name, so their values do not reach
#                   its logged command line.  The session runs in a database of its
#                   own (fb2_database), created and dropped by the suite.
# The mock-server suites are local either way.
TEST_TARGET = os.environ.get("FIREBOLT_TEST_TARGET", "core")
if TEST_TARGET not in ("core", "fb2"):
    raise pytest.UsageError(
        f"FIREBOLT_TEST_TARGET must be 'core' or 'fb2', got '{TEST_TARGET}'"
    )

FB2_REQUIRED_ENV = (
    "FIREBOLT_FB2_CLIENT_ID",
    "FIREBOLT_FB2_CLIENT_SECRET",
    "FIREBOLT_FB2_ACCOUNT",
    "FIREBOLT_FB2_ENGINE",
)


def fb2_configured() -> bool:
    return all(os.environ.get(name) for name in FB2_REQUIRED_ENV)


def fb2_db_kwargs(**overrides) -> dict:
    """FB2 only: database options for the configured Firebolt 2.0 SaaS engine."""
    kwargs = {
        "username": os.environ.get("FIREBOLT_FB2_CLIENT_ID", ""),
        "password": os.environ.get("FIREBOLT_FB2_CLIENT_SECRET", ""),
        "firebolt.account": os.environ.get("FIREBOLT_FB2_ACCOUNT", ""),
        "firebolt.engine": os.environ.get("FIREBOLT_FB2_ENGINE", ""),
        "firebolt.environment": os.environ.get("FIREBOLT_FB2_ENVIRONMENT") or "app",
        "firebolt.timeout_sec": "120",
    }
    kwargs.update(overrides)
    return {k: v for k, v in kwargs.items() if v is not None}


@pytest.fixture(scope="session")
def started_engine():
    product = firebolt_engine.FireboltInstance(__file__)
    product.add_engine(engine_name="engine1", num_nodes=1)
    product.start()
    try:
        yield product
    finally:
        product.stop()


@pytest.fixture(scope="session")
def server_url(started_engine):
    assert os.path.isfile(ADBC_DRIVER_PATH), (
        f"ADBC driver not found at {ADBC_DRIVER_PATH}"
    )
    node = started_engine.engines["engine1"].instances["node_1"]
    return f"http://{node.pg_host}:{firebolt_engine.QUERY_PORT}"


def _fb2_execute(sql: str) -> None:
    with (
        dbapi.connect(
            driver=ADBC_DRIVER_PATH, db_kwargs=fb2_db_kwargs(), autocommit=True
        ) as c,
        c.cursor() as cur,
    ):
        cur.execute(sql)


@pytest.fixture(scope="session")
def fb2_database():
    """FB2 only: a database of this session's own, adbc_test_<random>, dropped at
    the end, so runs never share or leave behind state in the account."""
    if not fb2_configured():
        pytest.fail("FB2 tests need " + ", ".join(FB2_REQUIRED_ENV), pytrace=False)
    name = f"adbc_test_{uuid.uuid4().hex[:8]}"
    _fb2_execute(f'CREATE DATABASE "{name}"')
    try:
        yield name
    finally:
        _fb2_execute(f'DROP DATABASE IF EXISTS "{name}"')


@pytest.fixture(scope="session")
def engine_db_kwargs(request):
    """Database options for the engine under test (see TEST_TARGET)."""
    if TEST_TARGET == "fb2":
        return fb2_db_kwargs(
            **{"firebolt.database": request.getfixturevalue("fb2_database")}
        )
    # Requested lazily, so the fb2 target never starts the local engine.
    return {"uri": request.getfixturevalue("server_url")}


@pytest.fixture
def conn(engine_db_kwargs):
    """A fresh AdbcConnection for each test."""
    with (
        adbc_driver_manager.AdbcDatabase(
            driver=ADBC_DRIVER_PATH, **engine_db_kwargs
        ) as db,
        adbc_driver_manager.AdbcConnection(db) as connection,
    ):
        yield connection


@pytest.fixture
def dbapi_conn(engine_db_kwargs):
    """A DBAPI-style Connection that opens its own AdbcDatabase + AdbcConnection.

    autocommit=True so each adbc_ingest() commits immediately and the data
    becomes visible to the read fixtures (which use a separate AdbcConnection).
    """
    with dbapi.connect(
        driver=ADBC_DRIVER_PATH, db_kwargs=engine_db_kwargs, autocommit=True
    ) as c:
        yield c


@pytest.fixture
def cursor(dbapi_conn):
    """A DBAPI Cursor — the public-API path (Cursor.adbc_ingest, execute, ...)."""
    with dbapi_conn.cursor() as cur:
        yield cur


@pytest.fixture
def ingest(conn):
    """Return a callable driving bulk ingest through the low-level AdbcStatement
    API, mirroring what the dbapi Cursor.adbc_ingest() wrapper does internally.
    Used to pin the C++ driver against the option set the wrapper sends."""

    def _ingest(
        table_name,
        data,
        mode="adbc.ingest.mode.create",
        *,
        catalog=None,
        db_schema=None,
    ):
        options = {
            adbc_driver_manager.StatementOptions.INGEST_MODE.value: mode,
            adbc_driver_manager.StatementOptions.INGEST_TARGET_TABLE.value: table_name,
        }
        if catalog is not None:
            options[
                adbc_driver_manager.StatementOptions.INGEST_TARGET_CATALOG.value
            ] = catalog
        if db_schema is not None:
            options[
                adbc_driver_manager.StatementOptions.INGEST_TARGET_DB_SCHEMA.value
            ] = db_schema

        with adbc_driver_manager.AdbcStatement(conn) as stmt:
            stmt.set_options(**options)
            # AdbcStatement.bind_stream accepts the ArrowArrayStream PyCapsule
            # produced by pyarrow's __arrow_c_stream__ method.
            stmt.bind_stream(data.__arrow_c_stream__())
            stmt.execute_update()

    return _ingest


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
def scratch_table(run_query, table_name: str) -> str:
    """A unique table name that is dropped on teardown, whether or not the test
    created it.  For tests that produce the table themselves (e.g. create-mode
    ingest) and so cannot use `temp_table`'s fixed schema."""
    yield table_name
    run_query(f"DROP TABLE IF EXISTS {table_name}")


@pytest.fixture
def mock_server():
    """An in-process HTTP server that lets a test inject crafted response
    status codes and headers — used by security regression tests that need
    to exercise scenarios a real Firebolt engine won't reproduce (e.g.
    server-injected Firebolt-Update-Parameters on a 5xx response).

    Function-scoped: each test gets a fresh queue and capture list.
    """
    server = MockFireboltServer()
    server.start()
    try:
        yield server
    finally:
        server.stop()


@pytest.fixture
def tls_mock_server():
    """mock_server over https://, with a certificate generated for it (`tls_cert`)."""
    server = MockFireboltServer(tls=True)
    server.start()
    try:
        yield server
    finally:
        server.stop()


@pytest.fixture
def conn_to_mock(mock_server):
    """An AdbcConnection pointed at the mock server instead of a real engine."""
    assert os.path.isfile(ADBC_DRIVER_PATH), (
        f"ADBC driver not found at {ADBC_DRIVER_PATH}"
    )
    with (
        adbc_driver_manager.AdbcDatabase(
            driver=ADBC_DRIVER_PATH, uri=mock_server.url
        ) as db,
        adbc_driver_manager.AdbcConnection(db) as connection,
    ):
        yield connection


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
