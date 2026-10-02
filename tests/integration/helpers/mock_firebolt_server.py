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

"""Tiny in-process HTTP server that pretends to be a Firebolt query endpoint.

Used by integration tests that need to exercise the driver against crafted
response headers / status codes — scenarios a real Firebolt Core won't
produce.  The server runs on a background thread bound to 127.0.0.1 inside
the runner container; the driver inside the same container connects via
http://127.0.0.1:<port>.

Usage from a test:

    def test_x(mock_server):
        mock_server.queue(status=500, body=b"oops",
                          headers=[("Firebolt-Update-Parameters", "database=evil")])
        # ... drive ADBC ...
        last = mock_server.last_request
        assert "database=evil" not in last.path
"""

import dataclasses
import http.server
import os
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
from collections import deque
from collections.abc import Iterable


@dataclasses.dataclass
class CapturedRequest:
    method: str
    path: str
    headers: dict
    body: bytes


@dataclasses.dataclass
class _QueuedResponse:
    status: int
    body: bytes
    headers: list  # list of (name, value) tuples — duplicates allowed


class MockFireboltServer:
    """Thread-controlled HTTP server with a response queue.

    Each `queue(...)` call enqueues one response that will be served on the
    next incoming request.  When the queue is empty the server returns 200 OK
    with empty body so the test can drive prologue / cleanup queries without
    micromanaging every request.
    """

    def __init__(self, tls: bool = False):
        self.tls = tls
        # With tls=True: a self-signed certificate for 127.0.0.1 / localhost,
        # generated for this server alone and deleted by stop(), so no key is ever
        # committed.  Clients trust it only when pointed at `tls_cert`.
        self.tls_cert = None
        self._tls_dir = None
        self._lock = threading.Lock()
        self._queued: deque[_QueuedResponse] = deque()
        self._captured: list[CapturedRequest] = []

        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, format, *args):  # silence default stderr logging
                pass

            def _read_body(self) -> bytes:
                length = int(self.headers.get("Content-Length", "0") or "0")
                return self.rfile.read(length) if length > 0 else b""

            def _serve(self):
                body = self._read_body()
                with outer._lock:
                    outer._captured.append(
                        CapturedRequest(
                            method=self.command,
                            path=self.path,
                            headers={k: v for k, v in self.headers.items()},
                            body=body,
                        )
                    )
                    response = (
                        outer._queued.popleft()
                        if outer._queued
                        else _QueuedResponse(200, b"", [])
                    )
                self.send_response(response.status)
                self.send_header("Content-Length", str(len(response.body)))
                for name, value in response.headers:
                    self.send_header(name, value)
                self.end_headers()
                if response.body:
                    self.wfile.write(response.body)

            def do_GET(self):
                self._serve()

            def do_POST(self):
                self._serve()

            def do_PUT(self):
                self._serve()

        # Bind to port 0 to let the kernel pick a free port.
        self._server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        if tls:
            self._tls_dir = tempfile.mkdtemp(prefix="mock-firebolt-tls-")
            self.tls_cert = os.path.join(self._tls_dir, "cert.pem")
            key = os.path.join(self._tls_dir, "key.pem")
            _generate_self_signed_certificate(self.tls_cert, key)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(self.tls_cert, key)
            self._server.socket = context.wrap_socket(
                self._server.socket, server_side=True
            )
        self.host, self.port = self._server.server_address
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def url(self) -> str:
        return f"{'https' if self.tls else 'http'}://{self.host}:{self.port}"

    def start(self):
        self._thread.start()

    def stop(self):
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=2.0)
        if self._tls_dir:
            shutil.rmtree(self._tls_dir, ignore_errors=True)

    # ----- test-side controls -----------------------------------------------

    def queue(
        self, *, status: int = 200, body: bytes = b"", headers: Iterable[tuple] = ()
    ):
        """Queue one response to be served on the next request."""
        with self._lock:
            self._queued.append(_QueuedResponse(status, body, list(headers)))

    def reset(self):
        """Drop all queued responses and captured requests."""
        with self._lock:
            self._queued.clear()
            self._captured.clear()

    @property
    def captured(self) -> list[CapturedRequest]:
        with self._lock:
            return list(self._captured)

    @property
    def last_request(self) -> CapturedRequest | None:
        with self._lock:
            return self._captured[-1] if self._captured else None


def _generate_self_signed_certificate(cert_path: str, key_path: str) -> None:
    """A one-day P-256 certificate for 127.0.0.1 and localhost, via the openssl CLI."""
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-nodes",
            "-days",
            "1",
            "-newkey",
            "ec",
            "-pkeyopt",
            "ec_paramgen_curve:prime256v1",
            "-subj",
            "/CN=firebolt-adbc-test",
            "-addext",
            "subjectAltName=IP:127.0.0.1,DNS:localhost",
            "-keyout",
            key_path,
            "-out",
            cert_path,
        ],
        check=True,
        capture_output=True,
    )


def find_free_port() -> int:
    """Return a free TCP port on 127.0.0.1.  Useful for picking ports up-front."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
