"""A tiny stand-in for Firebase Auth + Realtime Database REST/streaming, for tests.

Implements just what medicart_control.py uses:
  POST /v1/accounts:signInWithPassword, POST /v1/token
  GET  /<path>.json   (normal or  Accept: text/event-stream)
  PUT / PATCH /<path>.json   with {".sv": "timestamp"} resolved
Rules are checked like the real database.rules.json for carts/$cartId: auth != null.
"""
import json
import queue
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

USERS = {"cart01@medicart.iub": ("secret-pass", "uid-cart")}


class Store:
    def __init__(self):
        self.data = {}
        self.lock = threading.Lock()
        self.listeners = []          # (path_parts, queue)
        self.writes = []             # (method, path, body) log

    @staticmethod
    def parts(path):
        return [p for p in path.strip("/").split("/") if p]

    def get(self, path):
        node = self.data
        for p in self.parts(path):
            if not isinstance(node, dict) or p not in node:
                return None
            node = node[p]
        return node

    def _resolve(self, v):
        if isinstance(v, dict):
            if v == {".sv": "timestamp"}:
                return int(time.time() * 1000)
            return {k: self._resolve(x) for k, x in v.items()}
        return v

    def _set(self, parts, value):
        if not parts:
            self.data = value if isinstance(value, dict) else {}
            return
        node = self.data
        for p in parts[:-1]:
            node = node.setdefault(p, {})
            if not isinstance(node, dict):
                raise ValueError("not a dict")
        if value is None:
            node.pop(parts[-1], None)
        else:
            node[parts[-1]] = value

    def write(self, method, path, body):
        body = self._resolve(body)
        parts = self.parts(path)
        with self.lock:
            self.writes.append((method, path, body))
            if method == "PUT":
                self._set(parts, body)
                events = [("put", parts, body)]
            else:
                for k, v in body.items():
                    self._set(parts + self.parts(k), v)
                events = [("patch", parts, body)]
            for lparts, q in list(self.listeners):
                for ev, eparts, data in events:
                    if eparts[:len(lparts)] == lparts:
                        rel = "/" + "/".join(eparts[len(lparts):])
                        q.put((ev, {"path": rel, "data": data}))
                    elif lparts[:len(eparts)] == eparts:  # write above the listener
                        sub = self.get("/" + "/".join(lparts))
                        q.put(("put", {"path": "/", "data": sub}))


def make_server(port=0):
    store = Store()

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def end_headers(self):          # browsers (test pages) call from another origin
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, PUT, PATCH, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            super().end_headers()

        def do_OPTIONS(self):
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def _auth(self, qs):
            return qs.get("auth", [""])[0].startswith("tok-")

        def _json(self, code, obj):
            b = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def _body(self):
            n = int(self.headers.get("Content-Length", 0))
            raw = self.rfile.read(n) if n else b""
            ctype = self.headers.get("Content-Type", "")
            if "x-www-form-urlencoded" in ctype:
                return {k: v[0] for k, v in parse_qs(raw.decode()).items()}
            return json.loads(raw or b"null")

        def do_POST(self):
            u = urlparse(self.path)
            body = self._body()
            if u.path.endswith("accounts:signInWithPassword"):
                user = USERS.get(body.get("email"))
                if not user or user[0] != body.get("password"):
                    return self._json(400, {"error": {"message": "INVALID_LOGIN_CREDENTIALS"}})
                return self._json(200, {"idToken": "tok-1", "refreshToken": "ref-1", "expiresIn": "3600", "localId": user[1]})
            if u.path.endswith("/v1/token"):
                return self._json(200, {"id_token": "tok-2", "refresh_token": "ref-2", "expires_in": "3600"})
            self._json(404, {"error": "no route"})

        def do_GET(self):
            u = urlparse(self.path)
            qs = parse_qs(u.query)
            path = u.path[:-5] if u.path.endswith(".json") else u.path
            if not self._auth(qs):
                return self._json(401, {"error": "Permission denied"})
            if "text/event-stream" in self.headers.get("Accept", ""):
                q = queue.Queue()
                lparts = store.parts(path)
                with store.lock:
                    store.listeners.append((lparts, q))
                    q.put(("put", {"path": "/", "data": store.get(path)}))
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                try:
                    while True:
                        try:
                            ev, data = q.get(timeout=5)
                            msg = f"event: {ev}\ndata: {json.dumps(data)}\n\n"
                        except queue.Empty:
                            msg = "event: keep-alive\ndata: null\n\n"
                        self.wfile.write(msg.encode())
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    with store.lock:
                        store.listeners = [x for x in store.listeners if x[1] is not q]
                return
            self._json(200, store.get(path))

        def _write(self, method):
            u = urlparse(self.path)
            qs = parse_qs(u.query)
            path = u.path[:-5] if u.path.endswith(".json") else u.path
            if not self._auth(qs):
                return self._json(401, {"error": "Permission denied"})
            if store.parts(path)[:1] not in (["carts"], ["medicart"], ["calls"]):
                return self._json(401, {"error": "Permission denied"})
            body = self._body()
            store.write(method, path, body)
            self._json(200, body)

        def do_PUT(self):
            self._write("PUT")

        def do_PATCH(self):
            self._write("PATCH")

    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    srv.daemon_threads = True
    srv.handle_error = lambda *a: None
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, store
