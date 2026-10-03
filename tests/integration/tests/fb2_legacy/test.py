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
