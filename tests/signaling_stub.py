import json
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class SignalingStub:
    def __init__(self):
        self.lock = threading.Lock()
        self.tokens = {}
        self.calls = []
        self.response_gate = None
        self.verify_started = threading.Event()
        self.verify_finished = threading.Event()
        stub = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
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
                gate = stub.response_gate
                if gate is not None:
                    stub.verify_started.set()
                    gate.wait(timeout=10)
                try:
                    self.send_response(200 if accepted else 403)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    if gate is not None:
                        stub.verify_finished.set()

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
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

    def pending(self, token):
        with self.lock:
            return token in self.tokens

    def count(self):
        with self.lock:
            return len(self.calls)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        assert not self.thread.is_alive(), "signaling stub failed to stop"
