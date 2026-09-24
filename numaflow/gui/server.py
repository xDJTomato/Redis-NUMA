#!/usr/bin/env python3
"""server.py - tiny stdlib HTTP bridge for the NUMAflow web GUI.

Serves gui/index.html and exposes JSON endpoints backed by the compiled C11
`numaflow` binary (the GUI is a frontend; all core logic stays in C):

  GET  /api/ops        -> seven public actions (numaflow dump-ops)
  GET  /api/ops/legacy -> full catalog, for displaying imported templates
  GET  /api/templates  -> template choices
  POST /api/run        -> execute a workflow DAG (body = workflow JSON)
  GET  /               -> the editor

Run:  python3 gui/server.py   then open http://127.0.0.1:8090
"""
import http.server
import json
import os
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

def find_binary():
    for name in ("numaflow.exe", "numaflow"):
        p = os.path.join(ROOT, "build", name)
        if os.path.exists(p):
            return p
    return None

BIN = find_binary()
_ops_cache = {}

def get_ops(include_legacy=False):
    global _ops_cache
    if include_legacy in _ops_cache:
        return _ops_cache[include_legacy]
    if not BIN:
        return {"error": "numaflow binary not found; run make in numaflow/"}
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "ops.json")
        try:
            subprocess.run([BIN, "dump-ops", out] + (["--all"] if include_legacy else []),
                           check=False, capture_output=True, timeout=5)
        except subprocess.TimeoutExpired:
            return []
        if os.path.exists(out):
            with open(out, encoding="utf-8") as f:
                _ops_cache[include_legacy] = json.load(f)
        else:
            _ops_cache[include_legacy] = []
    return _ops_cache[include_legacy]

def get_templates():
    if not BIN:
        return [{"error": "numaflow binary not found"}]
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "templates.json")
        try:
            subprocess.run([BIN, "dump-templates", out], check=False,
                           capture_output=True, timeout=5)
        except subprocess.TimeoutExpired:
            return []
        if os.path.exists(out):
            with open(out, encoding="utf-8") as f:
                return json.load(f)
    return []

def get_template(name):
    if not BIN:
        return None
    try:
        p = subprocess.run([BIN, "template", name], capture_output=True,
                           text=True, timeout=5)
    except subprocess.TimeoutExpired:
        return None
    if p.returncode != 0:
        return None
    try:
        return json.loads(p.stdout)
    except Exception:
        return None

def run_workflow(body):
    if not BIN:
        return 503, "numaflow binary not found"
    with tempfile.TemporaryDirectory() as d:
        wf = os.path.join(d, "wf.json")
        with open(wf, "w", encoding="utf-8") as f:
            f.write(body)
        try:
            p = subprocess.run([BIN, "run", wf], capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            return 504, "workflow execution timed out"
        return (200 if p.returncode == 0 else 400), (p.stdout + p.stderr).strip()

class Handler(http.server.BaseHTTPRequestHandler):
    def _send(self, code, data, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            p = os.path.join(HERE, "index.html")
            if os.path.exists(p):
                with open(p, "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
                return
        if self.path in ("/style.css", "/app.js", "/i18n.js"):
            path = os.path.join(HERE, self.path[1:])
            with open(path, "rb") as f:
                self._send(200, f.read(), "text/css; charset=utf-8" if self.path.endswith(".css") else "text/javascript; charset=utf-8")
            return
        if self.path in ("/api/ops", "/api/ops/legacy"):
            self._send(200 if BIN else 503,
                       json.dumps(get_ops(self.path.endswith("/legacy"))).encode(),
                       "application/json")
            return
        if self.path == "/api/templates":
            self._send(200 if BIN else 503, json.dumps(get_templates()).encode(),
                       "application/json")
            return
        if self.path.startswith("/api/template/"):
            name = self.path[len("/api/template/"):]
            wf = get_template(name)
            if wf is None:
                self._send(404, b"not found", "text/plain")
            else:
                self._send(200, json.dumps(wf).encode(), "application/json")
            return
        if self.path == "/api/strategies":
            self._send(200, json.dumps({"strategies": ["caat","composite_lru","tinylfu","noop"]}).encode(), "application/json")
            return
        self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if self.path == "/api/run":
            try:
                n = int(self.headers.get("Content-Length", 0))
                if n <= 0 or n > 2_000_000:
                    raise ValueError("workflow JSON must be between 1 byte and 2 MB")
                body = self.rfile.read(n).decode("utf-8")
                json.loads(body)
            except (ValueError, UnicodeError):
                self._send(400, b"invalid workflow JSON", "text/plain; charset=utf-8")
                return
            code, out = run_workflow(body)
            self._send(code, out.encode("utf-8"), "text/plain; charset=utf-8")
            return
        self._send(404, b"not found", "text/plain")

    def log_message(self, *a):
        pass

def main():
    port = int(os.environ.get("PORT", "8090"))
    print("NUMAflow GUI server -> http://127.0.0.1:%d  (binary: %s)" % (port, BIN or "NOT FOUND"))
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()

if __name__ == "__main__":
    main()
