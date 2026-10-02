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

"""TLS: https:// endpoints are reachable and peer verification is strict.

Runs against tls_mock_server, an https:// mock serving a self-signed
certificate.  The certificate is trusted only when named through
firebolt.ssl_certificate_path, so these tests show both that the option
works and that an untrusted peer is refused.
"""

import io

import adbc_driver_manager
import pyarrow as pa
import pyarrow.ipc
import pytest

from conftest import ADBC_DRIVER_PATH


def _arrow_body() -> bytes:
    sink = io.BytesIO()
    table = pa.table({"x": pa.array([1], pa.int32())})
    with pyarrow.ipc.new_stream(sink, table.schema) as writer:
        writer.write_table(table)
    return sink.getvalue()


def _select_one(server, **db_kwargs) -> pa.Table:
    with adbc_driver_manager.AdbcDatabase(driver=ADBC_DRIVER_PATH, uri=server.url, **db_kwargs) as db:
        with adbc_driver_manager.AdbcConnection(db) as conn:
            with adbc_driver_manager.AdbcStatement(conn) as stmt:
                stmt.set_sql_query("SELECT 1")
                stream, _ = stmt.execute_query()
                return pa.RecordBatchReader._import_from_c(stream.address).read_all()


def test_https_query_with_configured_ca_bundle(tls_mock_server):
    tls_mock_server.queue(body=_arrow_body())
    table = _select_one(tls_mock_server, **{"firebolt.ssl_certificate_path": tls_mock_server.tls_cert})
    assert table.column("x").to_pylist() == [1]


def test_untrusted_certificate_is_refused(tls_mock_server):
    # The system bundle does not contain the self-signed test certificate.
    with pytest.raises(adbc_driver_manager.Error) as exc:
        _select_one(tls_mock_server)
    assert "certificate" in str(exc.value).lower() or "ssl" in str(exc.value).lower(), str(exc.value)
    assert tls_mock_server.captured == [], "no request may reach an unverified peer"


def test_missing_ca_bundle_rejected_at_init(tls_mock_server):
    with pytest.raises(adbc_driver_manager.ProgrammingError) as exc:
        _select_one(tls_mock_server, **{"firebolt.ssl_certificate_path": "/no/such/ca.pem"})
    assert "/no/such/ca.pem" in str(exc.value)
