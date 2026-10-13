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

"""Query results are streamed, not buffered.

ExecuteQuery returns once the response has started; record batches are read off
the connection as the caller asks for them.  The mock-server tests stream a
response in chunks — pausing it, or ending it with the error text Firebolt
appends when a query fails after its 200 — to observe that.
"""

import threading
import time
import urllib.parse

import adbc_driver_manager
import pyarrow as pa
import pytest
from conftest import ADBC_DRIVER_PATH

SCHEMA = pa.schema([("x", pa.int64())])
END_OF_STREAM = b"\xff\xff\xff\xff\x00\x00\x00\x00"
ERROR_TRAILER = b'{"errors":[{"description":"Division by zero"}]}\n'


def _batch(*values) -> bytes:
    """One Arrow IPC record batch message."""
    return pa.record_batch([pa.array(values, pa.int64())], schema=SCHEMA).serialize()


def _schema() -> bytes:
    """The Arrow IPC schema message that starts a stream."""
    return SCHEMA.serialize()


def _execute(stmt, sql="SELECT x FROM t"):
    """Run a query and return its result as a pyarrow RecordBatchReader."""
    stmt.set_sql_query(sql)
    stream, _ = stmt.execute_query()
    return pa.RecordBatchReader._import_from_c(stream.address)


def _values(batch) -> list:
    return batch.column(0).to_pylist()


def _query_params(path: str, name: str) -> list[str]:
    pairs = urllib.parse.parse_qsl(urllib.parse.urlparse(path).query)
    return [value for key, value in pairs if key == name]


@pytest.fixture
def connect_to_mock(mock_server):
    """Open connections to the mock with extra database options; closed on teardown."""
    opened = []

    def connect(**options):
        db = adbc_driver_manager.AdbcDatabase(
            driver=ADBC_DRIVER_PATH, uri=mock_server.url, **options
        )
        connection = adbc_driver_manager.AdbcConnection(db)
        opened.append((db, connection))
        return connection

    yield connect
    for db, connection in reversed(opened):
        connection.close()
        db.close()


def _statement(conn):
    return adbc_driver_manager.AdbcStatement(conn)


def test_first_batch_arrives_before_the_response_ends(mock_server, conn_to_mock):
    mock_server.queue(chunks=[_schema(), _batch(1, 2), 5.0, _batch(3), END_OF_STREAM])

    with _statement(conn_to_mock) as stmt:
        started = time.monotonic()
        reader = _execute(stmt)
        first = reader.read_next_batch()
        elapsed = time.monotonic() - started

        assert _values(first) == [1, 2]
        assert elapsed < 3, (
            f"the first batch took {elapsed:.1f}s: the response was buffered"
        )
        assert _values(reader.read_next_batch()) == [3]
        with pytest.raises(StopIteration):
            reader.read_next_batch()


def test_error_after_the_200_is_reported(mock_server, conn_to_mock):
    """Firebolt appends the error text after the last complete message, with no
    end-of-stream marker, when a query fails after the response started."""
    mock_server.queue(chunks=[_schema(), _batch(1), ERROR_TRAILER])

    with _statement(conn_to_mock) as stmt:
        reader = _execute(stmt)
        assert _values(reader.read_next_batch()) == [1]
        with pytest.raises(Exception, match="Division by zero"):
            reader.read_next_batch()


def test_error_after_the_200_is_reported_by_a_buffered_read(mock_server, conn_to_mock):
    """The same response, read all at once."""
    mock_server.queue(chunks=[_schema(), _batch(1), ERROR_TRAILER])

    with _statement(conn_to_mock) as stmt:
        reader = _execute(stmt)
        with pytest.raises(Exception, match="Division by zero"):
            reader.read_all()


def test_http_error_fails_execute(mock_server, conn_to_mock):
    mock_server.queue(status=400, body=b"syntax error at line 1")

    with (
        _statement(conn_to_mock) as stmt,
        pytest.raises(adbc_driver_manager.ProgrammingError, match="syntax error"),
    ):
        _execute(stmt, "SELEC 1")


def test_session_update_applies_before_the_stream_is_read(mock_server, conn_to_mock):
    gate = threading.Event()
    mock_server.queue(
        headers=[("Firebolt-Update-Parameters", "database=streamed_db")],
        chunks=[_schema(), gate, _batch(1), END_OF_STREAM],
    )
    with _statement(conn_to_mock) as stmt, _statement(conn_to_mock) as other:
        try:
            reader = _execute(stmt, "USE DATABASE streamed_db")
            # Another request while the first response is still open.
            _execute(other, "SELECT 1").read_all()
            assert _query_params(mock_server.captured[1].path, "database") == [
                "streamed_db"
            ]
        finally:
            gate.set()
        assert _values(reader.read_next_batch()) == [1]


def test_requests_while_a_stream_is_open(mock_server, conn_to_mock):
    """An open stream does not block the connection: other queries and a
    transaction's COMMIT run alongside it, and it still finishes intact."""
    conn_to_mock.set_options(**{"adbc.connection.autocommit": "false"})
    gate = threading.Event()
    mock_server.queue()  # BEGIN
    mock_server.queue(chunks=[_schema(), _batch(1), gate, _batch(2), END_OF_STREAM])
    mock_server.queue(chunks=[_schema(), _batch(7), END_OF_STREAM])
    with _statement(conn_to_mock) as stmt, _statement(conn_to_mock) as other:
        try:
            reader = _execute(stmt)
            assert _values(reader.read_next_batch()) == [1]

            assert _execute(other).read_all().column(0).to_pylist() == [7]
            conn_to_mock.commit()
            assert mock_server.last_request.body == b"COMMIT"
        finally:
            gate.set()
        assert _values(reader.read_next_batch()) == [2]
        with pytest.raises(StopIteration):
            reader.read_next_batch()


def test_releasing_an_unfinished_stream(mock_server, conn_to_mock):
    gate = threading.Event()
    mock_server.queue(chunks=[_schema(), _batch(1), gate, _batch(2), END_OF_STREAM])
    try:
        with _statement(conn_to_mock) as stmt:
            reader = _execute(stmt)
            assert _values(reader.read_next_batch()) == [1]
            reader.close()

        mock_server.queue(chunks=[_schema(), _batch(9), END_OF_STREAM])
        with _statement(conn_to_mock) as other:
            assert _execute(other).read_all().column(0).to_pylist() == [9]
    finally:
        gate.set()


def test_timeout_bounds_the_wait_for_the_server(mock_server, connect_to_mock):
    connection = connect_to_mock(**{"firebolt.timeout_sec": "1"})
    mock_server.queue(chunks=[3.0, _schema(), _batch(1), END_OF_STREAM])

    with (
        _statement(connection) as stmt,
        pytest.raises(
            adbc_driver_manager.OperationalError, match="firebolt.timeout_sec"
        ),
    ):
        _execute(stmt)


def test_timeout_does_not_bound_a_steady_stream(mock_server, connect_to_mock):
    """No pause is longer than the timeout, though the whole response is."""
    connection = connect_to_mock(**{"firebolt.timeout_sec": "1"})
    mock_server.queue(
        chunks=[
            _schema(),
            _batch(1),
            0.6,
            _batch(2),
            0.6,
            _batch(3),
            0.6,
            END_OF_STREAM,
        ]
    )

    with _statement(connection) as stmt:
        assert _execute(stmt).read_all().column(0).to_pylist() == [1, 2, 3]


def test_timeout_does_not_count_the_callers_time(mock_server, connect_to_mock):
    connection = connect_to_mock(**{"firebolt.timeout_sec": "1"})
    mock_server.queue(chunks=[_schema(), _batch(1), _batch(2), END_OF_STREAM])

    with _statement(connection) as stmt:
        reader = _execute(stmt)
        assert _values(reader.read_next_batch()) == [1]
        time.sleep(2)
        assert _values(reader.read_next_batch()) == [2]


def test_large_result_arrives_in_batches(conn):
    rows = 5_000_000
    with _statement(conn) as stmt:
        reader = _execute(stmt, f"SELECT i FROM generate_series(1, {rows}) AS g(i)")
        batches = 0
        total = 0
        for batch in reader:
            batches += 1
            total += batch.num_rows
    assert total == rows
    assert batches > 1
