#!/usr/bin/env python3
# PS5 DevLoop - embed the dashboard page into the payload.
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn tools/ps5web.html into payload/dashboard_html.h, so the resident payload
can serve the live dashboard itself (the console hosts it; no bridge needed).

    tools/embed_dashboard.py      -> payload/dashboard_html.h

Run it after editing the page, then rebuild the payload. The generated header is
committed so the build needs no extra step.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HTML = ROOT / "tools" / "ps5web.html"
OUT = ROOT / "payload" / "dashboard_html.h"


def main():
    data = HTML.read_bytes()
    rows = []
    for i in range(0, len(data), 16):
        rows.append("  " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    body = "\n".join(rows)
    OUT.write_text(
        "// Generated from tools/ps5web.html by tools/embed_dashboard.py.\n"
        "// Do not edit by hand; edit the HTML and regenerate.\n"
        "#ifndef DASHBOARD_HTML_H\n"
        "#define DASHBOARD_HTML_H\n\n"
        "static const unsigned char DASHBOARD_HTML[] = {\n"
        f"{body}\n"
        "};\n"
        f"static const unsigned int DASHBOARD_HTML_LEN = {len(data)}u;\n\n"
        "#endif\n",
        newline="\n",
    )
    print(f"wrote {OUT} ({len(data)} bytes of HTML)")


if __name__ == "__main__":
    main()
