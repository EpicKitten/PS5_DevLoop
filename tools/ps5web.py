#!/usr/bin/env python3
# PS5 DevLoop - a companion web dashboard that shows the console's hardware info
# live (model, temperatures, CPU clock).
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
"""Serve a small dashboard that polls the console's `hwinfo` and draws it live.

A browser cannot open the console's raw TCP port, so this is the bridge: it
serves the page and a /api/hwinfo endpoint that talks to the payload for it.

  tools/ps5web.py [--port N]     -> http://127.0.0.1:8099

The console's address comes from PS5_HOST or .env, as ps5loop does; it is never
sent to the browser.
"""

import argparse
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ps5loop import command, console_host  # reuse the console connection  # noqa: E402


def hwinfo():
    """Ask the payload for hwinfo and parse its one line into a dict."""
    answer, _ = command("hwinfo", timeout=10)
    if not answer.startswith("ok"):
        return {"ok": False, "error": answer}

    def grab(pattern, cast=str):
        found = re.search(pattern, answer)
        return cast(found.group(1)) if found else None

    return {
        "ok": True,
        "fw": grab(r"fw=(\S+)"),
        # model's value has a space ("CFI-2015 B01X"), so read up to " serial=".
        "model": grab(r"model=(.*?) serial="),
        "serial": grab(r"serial=(\S+)"),
        "cpuC": grab(r"cpuC=(-?\d+)", int),
        "socC": grab(r"socC=(-?\d+)", int),
        "cpuMHz": grab(r"cpuMHz=(-?\d+)", int),
    }


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self._send(200, "text/html; charset=utf-8", (HERE / "ps5web.html").read_bytes())
        elif self.path == "/api/hwinfo":
            try:
                data = hwinfo()
            except SystemExit as error:
                data = {"ok": False, "error": str(error)}
            except OSError as error:
                data = {"ok": False, "error": f"{error}"}
            self._send(200, "application/json", json.dumps(data).encode())
        else:
            self._send(404, "text/plain; charset=utf-8", b"not found")

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
    print(f"PS5 DevLoop dashboard: http://127.0.0.1:{args.port}  (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
