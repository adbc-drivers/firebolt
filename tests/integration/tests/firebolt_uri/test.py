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

"""Connecting with the driver's own firebolt:// scheme.

firebolt://<host>[:<port>]/[<database>]?ssl_mode=<mode> resolves to the HTTP
endpoint at Init: the path names the database and ssl_mode picks the transport
(verify-full by default, disable for plaintext). The test engine is plaintext;
the default goes to the https:// mock instead.
"""

import io
from urllib.parse import urlsplit

import adbc_driver_manager
import pyarrow as pa
import pyarrow.ipc
from adbc_driver_manager import dbapi
from conftest import ADBC_DRIVER_PATH


def _firebolt_uri(http_url: str, path: str = "/") -> str:
    return f"firebolt://{urlsplit(http_url).netloc}{path}?ssl_mode=disable"


def test_query_over_firebolt_uri(server_url):
    with (
        dbapi.connect(
            driver=ADBC_DRIVER_PATH, db_kwargs={"uri": _firebolt_uri(server_url)}
        ) as conn,
        conn.cursor() as cur,
    ):
        cur.execute("SELECT 42 AS answer")
        assert cur.fetchall() == [(42,)]


def test_path_names_the_database(mock_server):
    with (
        adbc_driver_manager.AdbcDatabase(
            driver=ADBC_DRIVER_PATH, uri=_firebolt_uri(mock_server.url, "/analytics")
        ) as db,
        adbc_driver_manager.AdbcConnection(db) as conn,
        adbc_driver_manager.AdbcStatement(conn) as stmt,
    ):
        stmt.set_sql_query("SELECT 1")
        try:
            stmt.execute_query()
        except adbc_driver_manager.OperationalError:
            pass  # the mock's empty 200 body is not an Arrow stream; only the request matters

    assert "database=analytics" in mock_server.last_request.path


def test_tls_is_the_default(tls_mock_server):
    sink = io.BytesIO()
    table = pa.table({"x": pa.array([1], pa.int32())})
    with pyarrow.ipc.new_stream(sink, table.schema) as writer:
        writer.write_table(table)
    tls_mock_server.queue(body=sink.getvalue())

    uri = f"firebolt://{urlsplit(tls_mock_server.url).netloc}/analytics"
    with (
        adbc_driver_manager.AdbcDatabase(
            driver=ADBC_DRIVER_PATH,
            uri=uri,
            **{"firebolt.ssl_certificate_path": tls_mock_server.tls_cert},
        ) as db,
        adbc_driver_manager.AdbcConnection(db) as conn,
        adbc_driver_manager.AdbcStatement(conn) as stmt,
    ):
        stmt.set_sql_query("SELECT 1")
        stream, _ = stmt.execute_query()
        assert pa.RecordBatchReader._import_from_c(stream.address).read_all().column(
            "x"
        ).to_pylist() == [1]

    assert "database=analytics" in tls_mock_server.last_request.path
