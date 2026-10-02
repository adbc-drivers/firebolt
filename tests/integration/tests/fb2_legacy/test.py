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

"""FB2 SaaS (Legacy) mode against a mock 2.0 control plane.

FB2 only.  The mock plays the token endpoint, the engineUrl API, the system
engine and the user engine; firebolt.auth_endpoint and
firebolt.api_endpoint (internal options) aim the driver at it.

The token cache is process-wide and this process loads the driver once, so
every test uses its own client ID: a token cached by one test must not
satisfy another.
"""

import json
import urllib.parse
import uuid

import adbc_driver_manager
import pytest
from conftest import ADBC_DRIVER_PATH

SECRET = "fbsec_DO_NOT_LEAK"


def _token_response(token="eyJ.mock.token", expires_in=7200):
    return {
        "status": 200,
        "body": json.dumps(
            {"access_token": token, "expires_in": expires_in, "token_type": "bearer"}
        ).encode(),
    }


def _options(mock, client_id, **overrides):
    options = {
        "username": client_id,
        "password": SECRET,
        "firebolt.account": "my_account",
        "firebolt.engine": "my_engine",
        "firebolt.auth_endpoint": f"{mock.url}/oauth/token",
        "firebolt.api_endpoint": mock.url,
        "firebolt.timeout_sec": "5",
    }
    options.update(overrides)
    return {k: v for k, v in options.items() if v is not None}


def _open_database(options):
    """Open and close a database; returns the error, if any."""
    try:
        with adbc_driver_manager.AdbcDatabase(driver=ADBC_DRIVER_PATH, **options):
            return None
    except adbc_driver_manager.Error as exc:
        return exc


def _token_requests(mock):
    return [
        r for r in mock.captured if urllib.parse.urlparse(r.path).path == "/oauth/token"
    ]


@pytest.fixture
def client_id():
    return f"fbcid_{uuid.uuid4().hex}"


def test_token_request_is_a_client_credentials_form(mock_server, client_id):
    mock_server.queue(**_token_response())
    _open_database(_options(mock_server, client_id))

    requests = _token_requests(mock_server)
    assert len(requests) == 1
    request = requests[0]
    assert request.method == "POST"
    assert request.headers.get("Content-Type", "").startswith(
        "application/x-www-form-urlencoded"
    )
    assert "Authorization" not in request.headers
    form = urllib.parse.parse_qs(request.body.decode())
    assert form == {
        "grant_type": ["client_credentials"],
        "audience": ["https://api.firebolt.io"],
        "client_id": [client_id],
        "client_secret": [SECRET],
    }
    assert SECRET not in request.path


def test_client_id_spelling_sends_the_same_credentials(mock_server, client_id):
    mock_server.queue(**_token_response())
    _open_database(
        _options(
            mock_server,
            client_id,
            username=None,
            password=None,
            **{"firebolt.client_id": client_id, "firebolt.client_secret": SECRET},
        )
    )
    form = urllib.parse.parse_qs(_token_requests(mock_server)[0].body.decode())
    assert form["client_id"] == [client_id] and form["client_secret"] == [SECRET]


def test_rejected_credentials_fail_init_as_unauthenticated(mock_server, client_id):
    mock_server.queue(
        status=401,
        body=json.dumps(
            {
                "error": "invalid_client",
                "error_description": "Client authentication failed",
            }
        ).encode(),
    )
    exc = _open_database(_options(mock_server, client_id))
    assert exc is not None
    assert exc.status_code == adbc_driver_manager.AdbcStatusCode.UNAUTHENTICATED, exc
    assert client_id in str(exc)
    assert SECRET not in str(exc)
    assert len(mock_server.captured) == 1, (
        "nothing past the token endpoint after a refusal"
    )


def test_token_is_cached_across_databases(mock_server, client_id):
    mock_server.queue(**_token_response())
    _open_database(_options(mock_server, client_id))
    mock_server.reset()
    _open_database(_options(mock_server, client_id))
    assert _token_requests(mock_server) == [], "a warm cache needs no token request"


def test_cache_connection_false_exchanges_every_time(mock_server, client_id):
    options = _options(mock_server, client_id, **{"firebolt.cache_connection": "false"})
    mock_server.queue(**_token_response())
    _open_database(options)
    mock_server.reset()
    mock_server.queue(**_token_response())
    _open_database(options)
    assert len(_token_requests(mock_server)) == 1


def test_pre_acquired_token_skips_the_exchange(mock_server):
    _open_database(
        _options(
            mock_server,
            client_id=None,
            username=None,
            password=None,
            **{"firebolt.token": "eyJ.pre.acquired"},
        )
    )
    assert _token_requests(mock_server) == []


# ---------------------------------------------------------------------------
# Engine resolution and the connection hooks.
#
# One mock plays every host; curl resolves *.localhost to loopback, so the
# driver sees distinct names for the token endpoint (id.), the control plane
# (api.), the system engine (sys.) and user engines (engine., other.), and the
# Host header tells the test which one a request was meant for.
# ---------------------------------------------------------------------------

import io

import pyarrow as pa
import pyarrow.ipc


def _arrow_body(value=1) -> bytes:
    sink = io.BytesIO()
    table = pa.table({"x": pa.array([value], pa.int32())})
    with pyarrow.ipc.new_stream(sink, table.schema) as writer:
        writer.write_table(table)
    return sink.getvalue()


def _host(mock, name):
    return f"{name}.localhost:{mock.port}"


def _named_options(mock, client_id, **overrides):
    return _options(
        mock,
        client_id,
        **{
            "firebolt.auth_endpoint": f"http://{_host(mock, 'id')}/oauth/token",
            "firebolt.api_endpoint": f"http://{_host(mock, 'api')}",
            "firebolt.database": "my_db",
            **overrides,
        },
    )


def _queue_bootstrap(mock, token="eyJ.tok.1", database=True):
    """Queue the bootstrap responses; token=None when the token cache is warm."""
    if token:
        mock.queue(**_token_response(token))
    mock.queue(status=200, body=json.dumps({"engineUrl": _host(mock, "sys")}).encode())
    if database:
        mock.queue(
            status=200,
            body=b"{}",
            headers=[("Firebolt-Update-Parameters", "database=my_db")],
        )
    mock.queue(
        status=200,
        body=b"{}",
        headers=[
            ("Firebolt-Reset-Session", ""),
            ("Firebolt-Update-Endpoint", f"{_host(mock, 'engine')}?engine=my_engine"),
        ],
    )


def _query(conn, sql="SELECT 1"):
    with adbc_driver_manager.AdbcStatement(conn) as stmt:
        stmt.set_sql_query(sql)
        stream, _ = stmt.execute_query()
        return pa.RecordBatchReader._import_from_c(stream.address).read_all()


class _Connected:
    """An open database + connection on the given options."""

    def __init__(self, options):
        self.db = adbc_driver_manager.AdbcDatabase(driver=ADBC_DRIVER_PATH, **options)
        self.conn = adbc_driver_manager.AdbcConnection(self.db)

    def __enter__(self):
        return self.conn

    def __exit__(self, *exc):
        self.conn.close()
        self.db.close()


def _parsed(request):
    url = urllib.parse.urlparse(request.path)
    return url.path, urllib.parse.parse_qs(url.query), request.headers.get("Host")


def test_connect_resolves_the_engine_then_queries_it(mock_server, client_id):
    _queue_bootstrap(mock_server)
    mock_server.queue(body=_arrow_body(7))
    with _Connected(_named_options(mock_server, client_id)) as conn:
        assert _query(conn).column("x").to_pylist() == [7]

    token, engine_url, use_db, use_engine, query = mock_server.captured
    assert _parsed(token)[0] == "/oauth/token" and _parsed(token)[2] == _host(
        mock_server, "id"
    )

    path, _, host = _parsed(engine_url)
    assert (engine_url.method, path, host) == (
        "GET",
        "/web/v3/account/my_account/engineUrl",
        _host(mock_server, "api"),
    )
    assert engine_url.headers["Authorization"] == "Bearer eyJ.tok.1"

    for request, sql in (
        (use_db, 'USE DATABASE "my_db"'),
        (use_engine, 'USE ENGINE "my_engine"'),
    ):
        path, params, host = _parsed(request)
        assert host == _host(mock_server, "sys")
        assert request.body.decode() == sql
        assert params.get("output_format") == ["JSON_Compact"]
        assert request.headers["Authorization"] == "Bearer eyJ.tok.1"

    _, params, host = _parsed(query)
    assert host == _host(mock_server, "engine")
    assert params["engine"] == ["my_engine"] and params["database"] == ["my_db"]
    assert query.headers["Authorization"] == "Bearer eyJ.tok.1"
    assert all(SECRET not in r.path for r in mock_server.captured)


def test_warm_cache_connects_with_no_bootstrap_requests(mock_server, client_id):
    _queue_bootstrap(mock_server)
    with _Connected(_named_options(mock_server, client_id)):
        pass
    mock_server.reset()
    mock_server.queue(body=_arrow_body())
    with _Connected(_named_options(mock_server, client_id)) as conn:
        _query(conn)
    assert [_parsed(r)[2] for r in mock_server.captured] == [
        _host(mock_server, "engine")
    ]


def test_unknown_account_is_not_found(mock_server, client_id):
    mock_server.queue(**_token_response())
    mock_server.queue(
        status=404,
        body=json.dumps(
            {"detail": "Requested resource has not been found", "kind": "not_found"}
        ).encode(),
    )
    exc = _open_database(_named_options(mock_server, client_id))
    assert exc.status_code == adbc_driver_manager.AdbcStatusCode.NOT_FOUND, exc
    assert "my_account" in str(exc)


def test_unknown_database_is_not_found(mock_server, client_id):
    mock_server.queue(**_token_response())
    mock_server.queue(
        status=200, body=json.dumps({"engineUrl": _host(mock_server, "sys")}).encode()
    )
    mock_server.queue(
        status=400,
        body=json.dumps(
            {
                "errors": [
                    {
                        "description": "Database 'my_db' does not exist or not authorized."
                    }
                ]
            }
        ).encode(),
    )
    exc = _open_database(_named_options(mock_server, client_id))
    assert exc.status_code == adbc_driver_manager.AdbcStatusCode.NOT_FOUND, exc
    assert "does not exist or not authorized" in str(exc)


def test_unknown_engine_is_not_found(mock_server, client_id):
    mock_server.queue(**_token_response())
    mock_server.queue(
        status=200, body=json.dumps({"engineUrl": _host(mock_server, "sys")}).encode()
    )
    mock_server.queue(
        status=400,
        body=json.dumps(
            {
                "errors": [
                    {
                        "description": "Engine 'my_engine' does not exist or not authorized."
                    }
                ]
            }
        ).encode(),
    )
    exc = _open_database(
        _named_options(mock_server, client_id, **{"firebolt.database": None})
    )
    assert exc.status_code == adbc_driver_manager.AdbcStatusCode.NOT_FOUND, exc
    assert "firebolt.engine" in str(exc)


def test_use_engine_without_endpoint_header_is_io(mock_server, client_id):
    mock_server.queue(**_token_response())
    mock_server.queue(
        status=200, body=json.dumps({"engineUrl": _host(mock_server, "sys")}).encode()
    )
    mock_server.queue(status=200, body=b"{}")
    exc = _open_database(
        _named_options(mock_server, client_id, **{"firebolt.database": None})
    )
    assert exc.status_code == adbc_driver_manager.AdbcStatusCode.IO, exc


def test_a_401_re_authenticates_once_and_retries(mock_server, client_id):
    _queue_bootstrap(mock_server, token="eyJ.tok.1")
    with _Connected(_named_options(mock_server, client_id)) as conn:
        mock_server.reset()
        mock_server.queue(status=401, body=b"Unauthorized")
        mock_server.queue(**_token_response("eyJ.tok.2"))
        mock_server.queue(body=_arrow_body(2))
        assert _query(conn).column("x").to_pylist() == [2]
        first, exchange, retry = mock_server.captured
        assert first.headers["Authorization"] == "Bearer eyJ.tok.1"
        assert _parsed(exchange)[0] == "/oauth/token"
        assert retry.headers["Authorization"] == "Bearer eyJ.tok.2"
        assert retry.body == first.body


def test_a_second_401_is_surfaced(mock_server, client_id):
    _queue_bootstrap(mock_server)
    with _Connected(_named_options(mock_server, client_id)) as conn:
        mock_server.reset()
        mock_server.queue(status=401, body=b"Unauthorized")
        mock_server.queue(**_token_response("eyJ.tok.2"))
        mock_server.queue(status=401, body=b"Unauthorized")
        with pytest.raises(adbc_driver_manager.Error) as exc:
            _query(conn)
        assert exc.value.status_code == adbc_driver_manager.AdbcStatusCode.UNAUTHORIZED
        assert len(mock_server.captured) == 3, "exactly one retry"


def test_a_pre_acquired_token_is_not_retried(mock_server):
    options = _named_options(
        mock_server,
        client_id=None,
        username=None,
        password=None,
        **{"firebolt.token": f"eyJ.{uuid.uuid4().hex}"},
    )
    mock_server.queue(
        status=200, body=json.dumps({"engineUrl": _host(mock_server, "sys")}).encode()
    )
    mock_server.queue(status=200, body=b"{}")
    mock_server.queue(
        status=200,
        body=b"{}",
        headers=[
            (
                "Firebolt-Update-Endpoint",
                f"{_host(mock_server, 'engine')}?engine=my_engine",
            )
        ],
    )
    with _Connected(options) as conn:
        mock_server.reset()
        mock_server.queue(status=401, body=b"Unauthorized")
        with pytest.raises(adbc_driver_manager.Error):
            _query(conn)
        assert len(mock_server.captured) == 1


def test_use_engine_mid_session_moves_only_that_connection(mock_server, client_id):
    _queue_bootstrap(mock_server)
    options = _named_options(mock_server, client_id)
    with _Connected(options) as conn:
        with _Connected(options) as other:
            mock_server.reset()
            mock_server.queue(
                status=200,
                body=_arrow_body(),
                headers=[
                    ("Firebolt-Reset-Session", ""),
                    (
                        "Firebolt-Update-Endpoint",
                        f"{_host(mock_server, 'other')}?engine=e2",
                    ),
                ],
            )
            _query(conn, "USE ENGINE e2")
            mock_server.queue(body=_arrow_body())
            _query(conn)
            mock_server.queue(body=_arrow_body())
            _query(other)
            _, moved, untouched = mock_server.captured
            assert _parsed(moved)[2] == _host(mock_server, "other") and _parsed(moved)[
                1
            ]["engine"] == ["e2"]
            assert _parsed(untouched)[2] == _host(mock_server, "engine")


def test_update_endpoint_to_a_foreign_host_is_refused(mock_server, client_id):
    _queue_bootstrap(mock_server)
    with _Connected(_named_options(mock_server, client_id)) as conn:
        mock_server.reset()
        mock_server.queue(
            status=200,
            body=_arrow_body(),
            headers=[
                ("Firebolt-Update-Endpoint", f"127.0.0.1:{mock_server.port}?engine=x")
            ],
        )
        with pytest.raises(adbc_driver_manager.Error) as exc:
            _query(conn, "USE ENGINE x")
        assert exc.value.status_code == adbc_driver_manager.AdbcStatusCode.IO
        assert "Firebolt-Update-Endpoint" in str(exc.value)
        mock_server.queue(body=_arrow_body())
        _query(conn)
        assert _parsed(mock_server.captured[-1])[2] == _host(mock_server, "engine"), (
            "endpoint unchanged"
        )


def test_stopped_engine_is_explained_and_forgotten(mock_server, client_id):
    _queue_bootstrap(mock_server)
    options = _named_options(mock_server, client_id)
    with _Connected(options) as conn:
        mock_server.reset()
        mock_server.queue(
            status=404,
            body=b"You are attempting to run a query on engine 'my_engine', but either it does not exist, it is stopped, "
            b"or you don't have permission to access it.",
        )
        with pytest.raises(adbc_driver_manager.Error) as exc:
            _query(conn)
        assert "START ENGINE" in str(exc.value)
    # The cached endpoint was dropped, so the next connect resolves again (the
    # token is still cached).
    mock_server.reset()
    _queue_bootstrap(mock_server, token=None)
    with _Connected(options):
        pass
    assert any(_parsed(r)[0].endswith("/engineUrl") for r in mock_server.captured)
