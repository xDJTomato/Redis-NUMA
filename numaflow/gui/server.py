#!/usr/bin/env python3
"""server.py - tiny stdlib HTTP bridge for the NUMAflow web GUI.

Serves gui/index.html and exposes JSON endpoints backed by the compiled C11
`numaflow` binary (the GUI is a frontend; all core logic stays in C):

  GET  /api/ops        -> atomic operation catalog (numaflow dump-ops)
  POST /api/run        -> execute a workflow DAG (body = workflow JSON)
  GET  /               -> the editor

Run:  python gui/server.py   then open http://127.0.0.1:8090
"""
import http.server, json, os, subprocess, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

def find_binary():
    for name in ("numaflow.exe", "numaflow"):
        p = os.path.join(ROOT, "build", name)
        if os.path.exists(p):
            return p
    return None

BIN = find_binary()
_ops_cache = None

def get_ops():
    global _ops_cache
    if _ops_cache is not None:
        return _ops_cache
    if not BIN:
        _ops_cache = {"error": "numaflow binary not found; run make in numaflow/"}
        return _ops_cache
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "ops.json")
        subprocess.run([BIN, "dump-ops", out], check=False, capture_output=True)
        if os.path.exists(out):
            with open(out, encoding="utf-8") as f:
                _ops_cache = json.load(f)
        else:
            _ops_cache = []
    return _ops_cache

def run_workflow(body):
    if not BIN:
        return "numaflow binary not found"
    with tempfile.TemporaryDirectory() as d:
        wf = os.path.join(d, "wf.json")
        with open(wf, "w", encoding="utf-8") as f:
            f.write(body)
        p = subprocess.run([BIN, "run", wf], capture_output=True, text=True, timeout=30)
        return (p.stdout + p.stderr).strip()

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
        if self.path == "/api/ops":
            self._send(200, json.dumps(get_ops()).encode(), "application/json")
            return
        if self.path == "/api/strategies":
            self._send(200, json.dumps({"strategies": ["caat","composite_lru","tinylfu","noop"]}).encode(), "application/json")
            return
        self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if self.path == "/api/run":
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n).decode("utf-8")
            out = run_workflow(body)
            self._send(200, out.encode("utf-8"), "text/plain; charset=utf-8")
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
