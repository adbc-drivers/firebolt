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

"""Compressed Arrow IPC results.

Every query asks for Arrow IPC with the codecs the driver can decode, and a
record batch compressed with either of them reads back intact.
"""

import adbc_driver_manager
import pyarrow as pa
import pytest
from conftest import ADBC_DRIVER_PATH

SCHEMA = pa.schema([("x", pa.int64()), ("s", pa.string())])
ACCEPT = 'application/vnd.apache.arrow.stream; codecs="zstd, lz4"'


def _stream(compression: str | None, *batches: pa.RecordBatch) -> bytes:
    sink = pa.BufferOutputStream()
    options = pa.ipc.IpcWriteOptions(compression=compression)
    with pa.ipc.new_stream(sink, SCHEMA, options=options) as writer:
        for batch in batches:
            writer.write_batch(batch)
    return sink.getvalue().to_pybytes()


def _batch(start: int, count: int) -> pa.RecordBatch:
    values = list(range(start, start + count))
    return pa.record_batch(
        [pa.array(values, pa.int64()), pa.array([f"row {v}" for v in values])],
        schema=SCHEMA,
    )


def _read(conn) -> pa.Table:
    with adbc_driver_manager.AdbcStatement(conn) as stmt:
        stmt.set_sql_query("SELECT x, s FROM t")
        stream, _ = stmt.execute_query()
        return pa.RecordBatchReader._import_from_c(stream.address).read_all()


def test_query_sends_codecs_in_accept_header(mock_server, conn_to_mock):
    """Checks the request only; the answer here is uncompressed (see the tests
    below for compressed answers)."""
    mock_server.queue(body=_stream(None, _batch(0, 1)))
    _read(conn_to_mock)
    headers = {k.lower(): v for k, v in mock_server.last_request.headers.items()}
    assert headers.get("accept") == ACCEPT


def _accept_sent_with(mock_server, value: str) -> str:
    mock_server.queue(body=_stream(None, _batch(0, 1)))
    with (
        adbc_driver_manager.AdbcDatabase(
            driver=ADBC_DRIVER_PATH,
            uri=mock_server.url,
            **{"firebolt.arrow_compression": value},
        ) as db,
        adbc_driver_manager.AdbcConnection(db) as conn,
    ):
        _read(conn)
    headers = {k.lower(): v for k, v in mock_server.last_request.headers.items()}
    return headers.get("accept")


@pytest.mark.parametrize(
    "value, accept",
    [
        ("any", ACCEPT),
        ("none", "application/vnd.apache.arrow.stream"),
        ("zstd", 'application/vnd.apache.arrow.stream; codecs="zstd"'),
        ("lz4", 'application/vnd.apache.arrow.stream; codecs="lz4"'),
        ("lz4,zstd", 'application/vnd.apache.arrow.stream; codecs="lz4, zstd"'),
        (" zstd , lz4 ", ACCEPT),
    ],
)
def test_arrow_compression_value_sets_accept_codecs(mock_server, value, accept):
    """A codec list is sent as given; the server picks among those codecs."""
    assert _accept_sent_with(mock_server, value) == accept


@pytest.mark.parametrize(
    "value",
    ["gzip", "zstd,gzip", "zstd,,lz4", "zstd,zstd", "", "true", "false", "all", "ANY"],
)
def test_arrow_compression_rejects_other_values(mock_server, value):
    with pytest.raises(adbc_driver_manager.Error, match="arrow_compression") as exc:
        adbc_driver_manager.AdbcDatabase(
            driver=ADBC_DRIVER_PATH,
            uri=mock_server.url,
            **{"firebolt.arrow_compression": value},
        )
    assert exc.value.status_code == adbc_driver_manager.AdbcStatusCode.INVALID_ARGUMENT


@pytest.mark.parametrize("codec", ["zstd", "lz4"])
def test_compressed_batches_are_read(mock_server, conn_to_mock, codec):
    """The mock sends the batches compressed with `codec`: pyarrow compresses each
    batch's buffers, so the body is smaller than the same batches uncompressed.
    (A driver without the codec fails here with nanoarrow's "Compression type ...
    not supported".)"""
    batches = [_batch(0, 5000), _batch(5000, 3)]
    body = _stream(codec, *batches)
    uncompressed = _stream(None, *batches)
    assert len(body) < len(uncompressed), (
        f"{codec} body is {len(body)} bytes, uncompressed {len(uncompressed)}"
    )
    mock_server.queue(body=body)

    assert _read(conn_to_mock) == pa.Table.from_batches(batches)


def test_real_engine_query_with_accept_header_works(conn):
    """The engine accepts the header (it may answer uncompressed)."""
    with adbc_driver_manager.AdbcStatement(conn) as stmt:
        stmt.set_sql_query("SELECT i FROM generate_series(1, 10000) AS g(i)")
        stream, _ = stmt.execute_query()
        table = pa.RecordBatchReader._import_from_c(stream.address).read_all()
    assert table.num_rows == 10000
