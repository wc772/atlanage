
import os
import sys
import http.server

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
REPO = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "仓库")


class AtpHandler(http.server.BaseHTTPRequestHandler):
    def _path(self):
        
        import urllib.parse
        return urllib.parse.unquote(self.path.split("?")[0]).lstrip("/")

    def do_GET(self):
        rel = self._path()
        if not rel:
            body = "atp 远程仓库（GET /<包名>/atp.json | /<包名>@<版本>/... | /<包名>.zip）".encode("utf-8")
            self._send(200, body, "text/plain; charset=utf-8")
            return
        
        fp = os.path.normpath(os.path.join(REPO, rel))
        if not fp.startswith(os.path.normpath(REPO)):
            self._send(403, b"forbidden", "text/plain")
            return
        if os.path.isfile(fp):
            with open(fp, "rb") as f:
                data = f.read()
            ctype = "application/zip" if rel.endswith(".zip") else "text/plain; charset=utf-8"
            self._send(200, data, ctype)
        else:
            self._send(404, b"not found: " + rel.encode("utf-8"), "text/plain")

    def do_POST(self):
        
        rel = self._path()
        if not rel.endswith(".zip"):
            self._send(400, "POST 只接受 <包名>.zip".encode("utf-8"), "text/plain")
            return
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0:
            self._send(400, b"empty body", "text/plain")
            return
        data = self.rfile.read(length)
        dst = os.path.normpath(os.path.join(REPO, rel))
        if not dst.startswith(os.path.normpath(REPO)):
            self._send(403, b"forbidden", "text/plain")
            return
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        
        import zipfile, io
        try:
            zf = zipfile.ZipFile(io.BytesIO(data))
            base = rel[:-4]  
            for name in zf.namelist():
                zf.extract(name, os.path.join(REPO, base))
        except Exception:
            pass
        self._send(200, b"ok", "text/plain")

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("[atp-server] %s\n" % (fmt % args))


if __name__ == "__main__":
    os.makedirs(REPO, exist_ok=True)
    print("atp 远程仓库: http://127.0.0.1:%d/  目录: %s" % (PORT, REPO))
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), AtpHandler).serve_forever()
