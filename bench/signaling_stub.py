import json
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class StubServer(ThreadingHTTPServer):
    request_queue_size = 4096


class SignalingStub:
    def __init__(self):
        self.lock = threading.Lock()
        self.tokens = {}
        self.calls = []
        self.handle_seconds = []
        self.rejected = 0
        stub = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                started = time.monotonic()
                try:
                    body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
                except (ValueError, TypeError):
                    body = {}
                with stub.lock:
                    stub.calls.append(body)
                    token = body.get("token")
                    expected = stub.tokens.get(token)
                    accepted = (self.path == "/internal/verify" and expected is not None
                                and expected[:2] == (body.get("operation"), body.get("stream_id"))
                                and (expected[2] is None or time.monotonic() < expected[2]))
                    if accepted:
                        del stub.tokens[token]
                try:
                    self.send_response(200 if accepted else 403)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                with stub.lock:
                    stub.handle_seconds.append(time.monotonic() - started)
                    stub.rejected += not accepted

            def log_message(self, *_):
                pass

        self.server = StubServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server.server_port}"

    def issue(self, operation, stream_id=None, ttl=None):
        token = secrets.token_hex(32)
        with self.lock:
            self.tokens[token] = (operation, token if stream_id is None else stream_id,
                                  None if ttl is None else time.monotonic() + ttl)
        return token

    def count(self):
        with self.lock:
            return len(self.calls)

    def summary(self):
        with self.lock:
            handled = sorted(self.handle_seconds)
            return {"calls": len(self.calls), "rejected": self.rejected,
                    "handle_ms_p99": handled[int(len(handled) * 0.99)] * 1000 if handled else None,
                    "handle_ms_max": handled[-1] * 1000 if handled else None}

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        assert not self.thread.is_alive(), "signaling stub failed to stop"
