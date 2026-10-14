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

A response can also be streamed with `chunks=[...]` instead of `body=`, so a
test can observe what the driver does with a response that has started but not
finished.  Each item of the list is one `Chunk`:

    bytes            sent at once, as one chunk of `Transfer-Encoding: chunked`
    threading.Event  the response waits until the test sets it
    Pause(seconds)   the response waits that long — only for tests about time,
                     such as timeouts; a test that just needs the response to
                     stop until it says so uses an Event
    Disconnect()     the connection closes there, without the final chunk: the
                     response breaks off, as when a server dies mid-response.
                     Its optional `closed` event is set once that has happened.

    gate = threading.Event()
    mock_server.queue(chunks=[schema, batch1, gate, batch2, end_of_stream])
    # ... read batch1 while the server is held at the gate ...
    gate.set()
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
import time
from collections import deque
from collections.abc import Iterable
from typing import TypeAlias


@dataclasses.dataclass
class CapturedRequest:
    method: str
    path: str
    headers: dict
    body: bytes


@dataclasses.dataclass(frozen=True)
class Pause:
    """A chunk item: the streamed response waits `seconds` before going on."""

    seconds: float


@dataclasses.dataclass(frozen=True)
class Disconnect:
    """A chunk item: the connection closes, without the final chunk."""

    closed: threading.Event | None = None


# One item of a streamed response; see the module docstring.
Chunk: TypeAlias = bytes | threading.Event | Pause | Disconnect


@dataclasses.dataclass
class _QueuedResponse:
    status: int
    body: bytes
    headers: list  # list of (name, value) tuples — duplicates allowed
    # Streamed instead of `body` when set.
    chunks: list[Chunk] | None = None


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
                if response.chunks is not None:
                    self._send_chunked(response)
                    return
                self.send_response(response.status)
                self.send_header("Content-Length", str(len(response.body)))
                for name, value in response.headers:
                    self.send_header(name, value)
                self.end_headers()
                if response.body:
                    self.wfile.write(response.body)

            def _send_chunked(self, response: _QueuedResponse):
                """Stream `response.chunks` the way Firebolt streams a result.

                With chunked transfer coding (RFC 9112 §7.1) the body has no
                Content-Length: it is a series of chunks, each its size in hex
                and CRLF, then the data and CRLF, ended by a zero-size chunk.
                The client sees each chunk as soon as it is flushed, so pausing
                between them leaves a response started but not finished."""
                # Chunked transfer coding needs an HTTP/1.1 status line.
                # One response per connection keeps the mock simple.
                self.protocol_version = "HTTP/1.1"
                self.close_connection = True
                self.send_response(response.status)
                self.send_header("Transfer-Encoding", "chunked")
                self.send_header("Connection", "close")
                for name, value in response.headers:
                    self.send_header(name, value)
                self.end_headers()
                try:
                    for item in response.chunks:
                        if isinstance(item, threading.Event):
                            # A bound, so a test that fails before setting its
                            # gate does not leave this thread waiting forever.
                            item.wait(timeout=60)
                        elif isinstance(item, Pause):
                            time.sleep(item.seconds)
                        elif isinstance(item, Disconnect):
                            self.wfile.flush()
                            self.connection.shutdown(socket.SHUT_WR)
                            if item.closed:
                                item.closed.set()
                            return

                        elif item:  # an empty chunk would end the body early
                            # One chunk: "<size in hex>\r\n<data>\r\n".
                            self.wfile.write(b"%x\r\n%s\r\n" % (len(item), item))
                            self.wfile.flush()
                    # The zero-size last chunk, then the empty line that ends
                    # the (absent) trailer section: the body is complete.
                    self.wfile.write(b"0\r\n\r\n")
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass  # the client stopped reading — what some tests do

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
        self,
        *,
        status: int = 200,
        body: bytes = b"",
        headers: Iterable[tuple] = (),
        chunks: Iterable[Chunk] | None = None,
    ):
        """Queue one response to be served on the next request.

        With `chunks`, the response is streamed instead of sent as `body` (see
        the module docstring)."""
        with self._lock:
            self._queued.append(
                _QueuedResponse(
                    status,
                    body,
                    list(headers),
                    list(chunks) if chunks is not None else None,
                )
            )

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
