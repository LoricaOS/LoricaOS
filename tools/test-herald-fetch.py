#!/usr/bin/env python3
"""Exercise production herald_fetch against a disposable local HTTP server."""
import http.server
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading


class Server(http.server.BaseHTTPRequestHandler):
    payload = b""
    ranges = []
    status = 200

    def do_GET(self):
        request_range = self.headers.get("Range")
        self.ranges.append(request_range)
        if self.status != 200:
            self.send_error(self.status)
            return
        start = int(request_range.split("=")[1].split("-")[0]) if request_range else 0
        if request_range and start >= len(self.payload):
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{len(self.payload)}")
            self.end_headers()
            return
        self.send_response(206 if request_range else 200)
        if request_range:
            self.send_header("Content-Range", f"bytes {start}-{len(self.payload)-1}/{len(self.payload)}")
        self.send_header("Content-Length", str(len(self.payload) - start))
        self.end_headers()
        self.wfile.write(self.payload[start:])

    def log_message(self, *_args):
        pass


def main():
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="herald-fetch-") as tmp:
        tmp = Path(tmp)
        driver = tmp / "driver.c"
        driver.write_text('#include "net.h"\nint main(int n, char **v) {\n'
                          'return n == 3 && herald_fetch(v[1], v[2]) == 0 ? 0 : 1;\n}\n')
        binary = tmp / "fetch"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
            "-I" + str(root / "user/bin/herald"), str(driver),
            str(root / "user/bin/herald/net.c"), "-o", str(binary)], check=True)
        server = http.server.HTTPServer(("127.0.0.1", 0), Server)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/object"
            output = tmp / "cache"
            # Same-sized, larger and smaller objects must all replace old bytes.
            for payload in (b"first object\n", b"other object\n",
                            b"a substantially larger replacement object\n", b"tiny\n"):
                Server.payload = payload
                subprocess.run([str(binary), url, str(output)], check=True, timeout=15)
                assert output.read_bytes() == payload, "fetch retained bytes from an older object"
            # A preexisting incomplete/corrupt cache is not trusted as a prefix.
            output.write_bytes(b"wrong")
            subprocess.run([str(binary), url, str(output)], check=True, timeout=15)
            assert output.read_bytes() == Server.payload
            assert not any(Server.ranges), "mutable fetch must not send a Range request"
            Server.payload = b""
            assert subprocess.run([str(binary), url, str(output)], timeout=15).returncode != 0
            Server.status = 404
            assert subprocess.run([str(binary), url, str(output)], timeout=15).returncode != 0
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
    print("herald fetch: mutable objects, stale cache, empty/error responses PASS")


if __name__ == "__main__":
    main()
