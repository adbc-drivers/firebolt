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
from helpers.mock_firebolt_server import Disconnect, Pause

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
    gate = threading.Event()
    mock_server.queue(chunks=[_schema(), _batch(1, 2), gate, _batch(3), END_OF_STREAM])

    with _statement(conn_to_mock) as stmt:
        try:
            # The server holds the rest of the response until the gate opens, so
            # a driver that buffered the whole response would block here (until
            # the mock gives up waiting) instead of returning the first batch.
            started = time.monotonic()
            reader = _execute(stmt)
            first = reader.read_next_batch()
            elapsed = time.monotonic() - started
            assert not gate.is_set()
        finally:
            gate.set()

        assert _values(first) == [1, 2]
        assert elapsed < 10, f"the first batch took {elapsed:.1f}s: it was buffered"
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


def test_http_error_fails_execute(mock_server, conn_to_mock):
    mock_server.queue(status=400, body=b"syntax error at line 1")

    with (
        _statement(conn_to_mock) as stmt,
        pytest.raises(adbc_driver_manager.ProgrammingError, match="syntax error"),
    ):
        _execute(stmt, "SELEC 1")


def test_session_update_from_a_streamed_response(mock_server, conn_to_mock):
    mock_server.queue(
        headers=[("Firebolt-Update-Parameters", "database=streamed_db")],
        chunks=[_schema(), _batch(1), END_OF_STREAM],
    )
    with _statement(conn_to_mock) as stmt, _statement(conn_to_mock) as other:
        reader = _execute(stmt, "USE DATABASE streamed_db")
        assert reader.read_all().column(0).to_pylist() == [1]
        _execute(other, "SELECT 1").read_all()
    assert _query_params(mock_server.captured[1].path, "database") == ["streamed_db"]


def test_requests_while_a_stream_is_open_are_refused(mock_server, conn_to_mock):
    """A connection runs one request at a time, and an open result stream is one:
    other queries and COMMIT fail with INVALID_STATE until it ends."""
    conn_to_mock.set_options(**{"adbc.connection.autocommit": "false"})
    gate = threading.Event()
    mock_server.queue()  # BEGIN
    mock_server.queue(chunks=[_schema(), _batch(1), gate, _batch(2), END_OF_STREAM])
    with _statement(conn_to_mock) as stmt, _statement(conn_to_mock) as other:
        try:
            reader = _execute(stmt)
            assert _values(reader.read_next_batch()) == [1]

            with pytest.raises(adbc_driver_manager.ProgrammingError) as exc:
                _execute(other)
            assert (
                exc.value.status_code
                == adbc_driver_manager.AdbcStatusCode.INVALID_STATE
            )
            assert "result stream" in str(exc.value)
            with pytest.raises(adbc_driver_manager.ProgrammingError):
                conn_to_mock.commit()
        finally:
            gate.set()
        assert _values(reader.read_next_batch()) == [2]
        assert len(mock_server.captured) == 2, "the refused requests were not sent"


def test_a_stream_read_to_the_end_frees_the_connection(mock_server, conn_to_mock):
    """Without the reader being released: reading past the last batch is enough."""
    mock_server.queue(chunks=[_schema(), _batch(1), END_OF_STREAM])
    mock_server.queue(chunks=[_schema(), _batch(2), END_OF_STREAM])
    with _statement(conn_to_mock) as stmt, _statement(conn_to_mock) as other:
        reader = _execute(stmt)
        assert reader.read_all().column(0).to_pylist() == [1]
        assert _execute(other).read_all().column(0).to_pylist() == [2]


def test_error_text_before_a_broken_connection_is_reported(mock_server, conn_to_mock):
    """The error text and the connection breaking arrive together: the text is
    still read, and reported, before the broken transfer."""
    gate, closed = threading.Event(), threading.Event()
    mock_server.queue(
        chunks=[_schema(), _batch(1), gate, ERROR_TRAILER, Disconnect(closed)]
    )

    with _statement(conn_to_mock) as stmt:
        reader = _execute(stmt)
        assert _values(reader.read_next_batch()) == [1]
        # Nothing reads while the server sends the text and closes, so the driver
        # finds both waiting at its next read.
        gate.set()
        assert closed.wait(timeout=10)
        with pytest.raises(Exception, match="Division by zero"):
            reader.read_next_batch()


def test_executing_a_statement_again_replaces_its_open_result(
    mock_server, conn_to_mock
):
    """ADBC: a new execution invalidates the statement's previous result.  Its
    stream is abandoned rather than blocking the connection."""
    gate = threading.Event()
    mock_server.queue(chunks=[_schema(), _batch(1), gate, _batch(2), END_OF_STREAM])
    mock_server.queue(chunks=[_schema(), _batch(7), END_OF_STREAM])
    try:
        with _statement(conn_to_mock) as stmt:
            first = _execute(stmt)
            assert _values(first.read_next_batch()) == [1]

            second = _execute(stmt)
            assert second.read_all().column(0).to_pylist() == [7]
            with pytest.raises(Exception, match="replaced by a new execution"):
                first.read_next_batch()
    finally:
        gate.set()


def test_a_failed_stream_frees_the_connection(mock_server, connect_to_mock):
    """After a stream fails (here: a timeout), the connection is usable again
    without the reader being released: the usual recovery, a rollback, works."""
    connection = connect_to_mock(**{"firebolt.timeout_sec": "1"})
    connection.set_options(**{"adbc.connection.autocommit": "false"})
    never_answers = threading.Event()
    mock_server.queue()  # BEGIN
    mock_server.queue(chunks=[_schema(), _batch(1), never_answers, END_OF_STREAM])
    try:
        with _statement(connection) as stmt:
            reader = _execute(stmt)
            assert _values(reader.read_next_batch()) == [1]
            with pytest.raises(Exception, match="firebolt.timeout_sec"):
                reader.read_next_batch()

            connection.rollback()
            assert mock_server.last_request.body == b"ROLLBACK"
    finally:
        never_answers.set()


def test_a_batch_that_does_not_decode_frees_the_connection(mock_server, conn_to_mock):
    """A message that starts like Arrow IPC but does not decode fails the read,
    and the connection is free again at once, without the reader being released."""
    undecodable = b"\xff\xff\xff\xff" + (16).to_bytes(4, "little") + b"\x00" * 16
    gate = threading.Event()  # keeps the transfer open past the bad message
    mock_server.queue(chunks=[_schema(), _batch(1), undecodable, gate, END_OF_STREAM])
    mock_server.queue(chunks=[_schema(), _batch(7), END_OF_STREAM])

    try:
        with _statement(conn_to_mock) as stmt, _statement(conn_to_mock) as other:
            reader = _execute(stmt)
            assert _values(reader.read_next_batch()) == [1]
            with pytest.raises(pa.ArrowException):
                reader.read_next_batch()
            assert _execute(other).read_all().column(0).to_pylist() == [7]
    finally:
        gate.set()


def test_a_timeout_reading_the_schema_is_a_timeout(mock_server, connect_to_mock):
    connection = connect_to_mock(**{"firebolt.timeout_sec": "1"})
    schema = _schema()
    never_answers = threading.Event()
    mock_server.queue(chunks=[schema[:8], never_answers, schema[8:], END_OF_STREAM])

    try:
        with _statement(connection) as stmt:
            with pytest.raises(adbc_driver_manager.OperationalError) as exc:
                _execute(stmt)
            assert exc.value.status_code == adbc_driver_manager.AdbcStatusCode.TIMEOUT
    finally:
        never_answers.set()


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


# The timeout tests below need real time to pass: the option counts whole
# seconds, so each takes a little over one.


def test_timeout_bounds_the_wait_for_the_server(mock_server, connect_to_mock):
    connection = connect_to_mock(**{"firebolt.timeout_sec": "1"})
    never_answers = threading.Event()
    mock_server.queue(chunks=[never_answers, _schema(), _batch(1), END_OF_STREAM])

    try:
        with (
            _statement(connection) as stmt,
            pytest.raises(
                adbc_driver_manager.OperationalError, match="firebolt.timeout_sec"
            ),
        ):
            _execute(stmt)
    finally:
        never_answers.set()


def test_timeout_does_not_bound_a_steady_stream(mock_server, connect_to_mock):
    """No pause is longer than the timeout, though the whole response is."""
    connection = connect_to_mock(**{"firebolt.timeout_sec": "1"})
    mock_server.queue(
        chunks=[_schema(), _batch(1), Pause(0.6), _batch(2), Pause(0.6), END_OF_STREAM]
    )

    with _statement(connection) as stmt:
        assert _execute(stmt).read_all().column(0).to_pylist() == [1, 2]


def test_a_huge_timeout_means_no_limit(mock_server, connect_to_mock):
    """A timeout far beyond any real wait must not overflow into an instant one."""
    connection = connect_to_mock(**{"firebolt.timeout_sec": "10000000000"})
    mock_server.queue(chunks=[_schema(), Pause(0.3), _batch(1), END_OF_STREAM])

    with _statement(connection) as stmt:
        assert _execute(stmt).read_all().column(0).to_pylist() == [1]


def test_timeout_does_not_count_the_callers_time(mock_server, connect_to_mock):
    connection = connect_to_mock(**{"firebolt.timeout_sec": "1"})
    mock_server.queue(chunks=[_schema(), _batch(1), _batch(2), END_OF_STREAM])

    with _statement(connection) as stmt:
        reader = _execute(stmt)
        assert _values(reader.read_next_batch()) == [1]
        time.sleep(1.5)  # longer than the timeout, spent by the caller
        assert _values(reader.read_next_batch()) == [2]


# The engine tests ask for small blocks (max_block_size, forwarded as a session
# setting), so a modest result already arrives as many record batches.
SMALL_BLOCK_ROWS = 1000


@pytest.fixture
def small_block_conn(conn):
    conn.set_options(**{"firebolt.session.max_block_size": str(SMALL_BLOCK_ROWS)})
    return conn


def _reset_peak_rss() -> None:
    """Make the current memory use the process's peak (Linux: VmHWM)."""
    with open("/proc/self/clear_refs", "w") as clear_refs:
        clear_refs.write("5")


def _rss_kib(field: str) -> int:
    with open("/proc/self/status") as status:
        for line in status:
            if line.startswith(field + ":"):
                return int(line.split()[1])
    raise AssertionError(f"{field} missing from /proc/self/status")


def test_large_result_is_not_held_in_memory(small_block_conn):
    """Against the real engine: reading a result batch by batch, dropping each
    batch, keeps the process's peak memory far below the result's size.  A driver
    that buffered the response would hold all of it."""
    rows = 4_000_000
    result_bytes = rows * 8  # one BIGINT column: ~30 MiB of Arrow data
    with _statement(small_block_conn) as stmt:
        # Before execute: a buffering driver reads the whole result there.
        _reset_peak_rss()
        before = _rss_kib("VmRSS")
        reader = _execute(stmt, f"SELECT i FROM generate_series(1, {rows}) AS g(i)")
        total = 0
        for batch in reader:
            total += batch.num_rows
        growth = (_rss_kib("VmHWM") - before) * 1024
    assert total == rows
    assert growth < result_bytes // 4, (
        f"peak memory grew by {growth >> 20} MiB reading a {result_bytes >> 20} MiB result"
    )
