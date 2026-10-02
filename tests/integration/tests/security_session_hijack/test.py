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

"""Server-injected session-parameter hijack regression test.

A malicious or compromised upstream — or a generic-error proxy — can return a
non-2xx response with `Firebolt-Update-Parameters: database=evil`.  Before the
fix the driver applied those updates unconditionally, so a single 5xx silently
rebound `database=` for the rest of the connection's life and routed every
subsequent query to the attacker-named database.

These tests use the in-process mock_server fixture (see
helpers/mock_firebolt_server.py) to inject crafted response headers and
verify that the driver does NOT pick up session params from non-2xx
responses, and that legitimate 2xx updates still take effect.
"""

import urllib.parse

import adbc_driver_manager
import pytest


def _capture_query_param(captured_path: str, name: str) -> list[str]:
    """Return the values of `name` in the URL of a captured request, in order."""
    parsed = urllib.parse.urlparse(captured_path)
    pairs = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    return [v for (k, v) in pairs if k == name]


def _run_select_one(conn) -> None:
    with adbc_driver_manager.AdbcStatement(conn) as stmt:
        stmt.set_sql_query("SELECT 1")
        try:
            stmt.execute_query()
        except adbc_driver_manager.OperationalError:
            # Mock server returns empty body on 200 — the driver may surface
            # an Arrow-IPC parse error.  We only care about request capture.
            pass


def test_4xx_with_update_parameters_does_not_mutate_session(mock_server, conn_to_mock):
    """Server returns 500 + Firebolt-Update-Parameters: database=attacker.
    The driver must NOT apply that update; the next request must NOT carry
    `database=attacker` in its URL."""
    mock_server.queue(
        status=500,
        body=b"upstream error",
        headers=[("Firebolt-Update-Parameters", "database=attacker_owned_db")],
    )

    # First request: triggers the hijack attempt.
    with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
        stmt.set_sql_query("SELECT 1")
        with pytest.raises(Exception):
            stmt.execute_query()

    # Second request: the post-hijack canary.
    _run_select_one(conn_to_mock)

    # The mock server saw both requests.  Inspect the *second* one.
    assert len(mock_server.captured) >= 2
    second_path = mock_server.captured[1].path
    injected = _capture_query_param(second_path, "database")
    assert "attacker_owned_db" not in injected, (
        f"5xx-injected Firebolt-Update-Parameters leaked into next URL: {second_path}"
    )


def test_2xx_update_parameters_still_applied(mock_server, conn_to_mock):
    """Sanity: a legitimate 2xx with Firebolt-Update-Parameters still mutates
    session_params on the next request — the gate is not too strict."""
    mock_server.queue(
        status=200,
        body=b"",
        headers=[("Firebolt-Update-Parameters", "session_marker=on")],
    )
    _run_select_one(conn_to_mock)

    # Next request should carry session_marker=on in the URL.
    _run_select_one(conn_to_mock)

    assert len(mock_server.captured) >= 2
    second_path = mock_server.captured[1].path
    values = _capture_query_param(second_path, "session_marker")
    assert "on" in values, (
        f"legitimate 2xx Firebolt-Update-Parameters did not propagate; URL={second_path}"
    )


def test_reset_session_ignored_on_error(mock_server, conn_to_mock):
    """5xx + Firebolt-Reset-Session must NOT clear previously legitimately-set
    session params."""
    # Step 1: legitimate 2xx that establishes a session marker.
    mock_server.queue(
        status=200,
        body=b"",
        headers=[("Firebolt-Update-Parameters", "session_marker=keep_me")],
    )
    _run_select_one(conn_to_mock)

    # Step 2: 5xx that asks the driver to reset session.  The driver must NOT
    # honour the reset hint from a non-2xx response.
    mock_server.queue(
        status=503,
        body=b"upstream",
        headers=[("Firebolt-Reset-Session", "1")],
    )
    with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
        stmt.set_sql_query("SELECT 1")
        with pytest.raises(Exception):
            stmt.execute_query()

    # Step 3: post-reset canary.  The session_marker must still be on the URL.
    _run_select_one(conn_to_mock)

    assert len(mock_server.captured) >= 3
    third_path = mock_server.captured[2].path
    values = _capture_query_param(third_path, "session_marker")
    assert "keep_me" in values, (
        f"5xx + Firebolt-Reset-Session erased legitimate session state; URL={third_path}"
    )


def test_token_not_in_url(mock_server, conn_to_mock):
    """Belt-and-braces sanity tied to fix #1: setting a token must not put it
    in the URL.  Captured here in the same hijack suite because both fixes
    share the URL-leak attack surface."""
    conn_to_mock.set_options(**{"adbc.firebolt.token": "secret_jwt_value"})
    _run_select_one(conn_to_mock)

    assert mock_server.last_request is not None
    captured_url = mock_server.last_request.path
    assert "secret_jwt_value" not in captured_url, (
        f"bearer token leaked into URL: {captured_url}"
    )
