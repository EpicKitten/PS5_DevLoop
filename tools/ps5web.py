#!/usr/bin/env python3
# PS5 DevLoop - optional localhost proxy for the console-hosted dashboard.
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
"""The live dashboard is now hosted by the console itself: the payload answers
HTTP on its own port, so a browser can open it directly (see README).

This bridge is optional. It proxies the console's HTTP to 127.0.0.1, for when
you would rather view it on localhost, or keep the console's address out of the
browser. It forwards each request to the payload and relays the reply.

  tools/ps5web.py [--port N]     -> http://127.0.0.1:8099

The console's address comes from PS5_HOST or .env, as ps5loop does; it is never
sent to the browser.
"""

import argparse
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ps5loop import console_host, PORT  # reuse the console connection  # noqa: E402


def fetch(path):
    """Forward a GET to the console's HTTP and return (status, headers, body)."""
    with socket.create_connection((console_host(), PORT), timeout=10) as sock:
        sock.sendall(
            f"GET {path} HTTP/1.1\r\nHost: ps5\r\nConnection: close\r\n\r\n".encode())
        raw = b""
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            raw += chunk
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = int(lines[0].split(b" ")[1]) if len(lines[0].split(b" ")) > 1 else 502
    headers = {}
    for line in lines[1:]:
        key, _, value = line.partition(b":")
        headers[key.strip().lower().decode()] = value.strip().decode()
    return status, headers, body


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            status, headers, body = fetch(self.path)
        except (OSError, SystemExit) as error:
            body = str(error).encode()
            self._send(502, "text/plain; charset=utf-8", body)
            return
        self._send(status, headers.get("content-type", "application/octet-stream"), body)

    def _send(self, code, content_type, body):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):  # keep the console quiet
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8099)
    args = parser.parse_args()
    console_host()  # fail early, before the browser opens, if no host is set
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"PS5 DevLoop dashboard (proxy): http://127.0.0.1:{args.port}  (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
