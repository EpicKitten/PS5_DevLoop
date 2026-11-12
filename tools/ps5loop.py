#!/usr/bin/env python3
# PS5 DevLoop - the client of the resident input and screen-capture payload.
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
"""Press buttons on the console and see what its screen shows.

This is DevLoop's client cut down to what testing Playden games uses: the pad,
the screen, the console's files and processes, and its logs. The payload still
answers DevLoop's other commands; `raw` reaches them for bring-up.

Commands:
  load [ELF]            send the payload to the console's ELF loader (once a boot)
  ping                  is the payload there
  doctor                check the host, payload, loader and capture tools
  discover [--timeout S]  find consoles on this LAN by UDP broadcast
  wake                  wake from standby (UDP packet; needs PS5_WAKE_CREDENTIAL)
  raw WORDS...          send one command line as it is, print the answer

  open [USERID]         create the virtual pad, give it to the foreground user, close
                        the prompt the console raises, and disconnect any real
                        controller so that titles read the pad
  close [--all]         remove it (--all: every virtual pad)
  status                the pad's handle, device, user and last insert result
  pads                  every virtual pad's state. Up to 4 pads: --pad N (1-4) on open,
                        close, status, press, hold, release and seq picks one; pads 2-4
                        go to the next signed-in user without a pad and never
                        disconnect a real controller (for local multiplayer)
  press BUTTONS [--ms]  press and release: "cross", "l1+r1", "down"
  hold BUTTONS [--lx ..] hold buttons, sticks (0-255, 128 centre) and triggers
  release               let everything go
  seq STEPS...          steps in order: a button press ("down", "cross:300" for
                        300 ms), or "wait:500"

  screenshot [FILE]     the whole screen, system UI included, through the console's
                        own screenshot: about 3 s
  latest-capture [FILE] fetch the newest capture without pressing anything
  list-captures         list the console's newest captures
  delete-capture PATH   delete a leftover capture as the gallery does

  ps                    list the console's processes (with title ids)
  locate-title          what is in the foreground now (app id, pid, title id)
  close-app [APP_ID]    close a running app (app id from ps; else the big app)
  kill PID              end a process by pid (a game that close-app leaves behind)
  ls PATH               list a console directory
  get REMOTE [LOCAL]    download a console file (default: downloads/<name>)
  put LOCAL REMOTE      upload a local file to the console
  rm PATH               delete a console file
  mv SRC DST            move or rename a console file
  df                    free space per mounted volume
  hwinfo                model, firmware, temperatures, cpu clock, fan and SoC power
  kernel-log [MS] [FILE] dump the console's kernel log (what a crashing game left)
  klog-stream [MS]      stream the kernel log live (Ctrl+C to stop)

The console's address comes from PS5_HOST in the environment, then from .env in
this project (.env.example). It is never printed.
"""

import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORT = int(os.environ.get("PS5LOOP_PORT", "9112"))
LOADER_PORT = int(os.environ.get("PS5_LOADER_PORT", "9021"))
# What a picture is scaled down to, so that it can be looked at cheaply.
VIEW_WIDTH = 1280

BUTTONS = {
    "create": 0x1, "l3": 0x2, "r3": 0x4, "options": 0x8,
    "up": 0x10, "right": 0x20, "down": 0x40, "left": 0x80,
    "l2": 0x100, "r2": 0x200, "l1": 0x400, "r1": 0x800,
    "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000,
    "ps": 0x10000, "touchpad": 0x100000,
}


def env_file_host(text):
    for raw in text.splitlines():
        key, _, value = raw.strip().partition("=")
        if key.strip() == "PS5_HOST" and value.strip():
            return value.strip()
    return None


def console_host():
    host = os.environ.get("PS5_HOST")
    if host:
        return host
    local = ROOT / ".env"
    if local.is_file():
        host = env_file_host(local.read_text(encoding="utf-8"))
        if host:
            return host
    raise SystemExit("PS5_HOST is not set: export it, or copy .env.example to .env")


def env_value(key):
    """Read a value from the environment, then from .env."""
    val = os.environ.get(key)
    if val:
        return val
    local = ROOT / ".env"
    if local.is_file():
        for raw in local.read_text(encoding="utf-8").splitlines():
            name, _, value = raw.strip().partition("=")
            if name.strip() == key and value.strip():
                return value.strip()
    return None


def wake():
    """Wake the console from standby with the PS5 discovery WAKEUP packet (UDP
    9302). Needs the console's user credential in PS5_WAKE_CREDENTIAL (env or
    .env); the console wakes only for its own credential."""
    cred = env_value("PS5_WAKE_CREDENTIAL")
    if not cred:
        raise SystemExit("wake needs PS5_WAKE_CREDENTIAL (the console's user credential) "
                         "in the environment or .env")
    packet = ("WAKEUP * HTTP/1.1\n"
              "client-type:fl\n"
              "auth-type:R\n"
              "model:w\n"
              "app-type:r\n"
              f"user-credential:{cred}\n"
              "device-discovery-protocol-version:00030010\n")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(packet.encode(), (console_host(), 9302))
    finally:
        sock.close()
    print("wake packet sent (UDP 9302); the console wakes only if the credential matches")
    return 0


def command(line, timeout=30.0, payload=False):
    """Send one command. Returns the answer line, and the bytes after it for a file."""
    try:
        connection = socket.create_connection((console_host(), PORT), timeout=10)
    except OSError as error:
        raise SystemExit(f"ps5loop: the payload did not answer ({error.strerror or error}); "
                         "run: ps5loop.py load") from None
    with connection:
        connection.settimeout(timeout)
        connection.sendall(line.encode() + b"\n")
        data = b""
        while b"\n" not in data:
            chunk = connection.recv(65536)
            if not chunk:
                break
            data += chunk
        answer, _, rest = data.partition(b"\n")
        answer = answer.decode("utf-8", "replace")
        if not payload or not answer.startswith("ok file"):
            return answer, b""
        size = int(dict(word.split("=", 1) for word in answer.split() if "=" in word)["size"])
        while len(rest) < size:
            chunk = connection.recv(1 << 20)
            if not chunk:
                raise SystemExit(f"ps5loop: the file ended at {len(rest)} of {size} bytes")
            rest += chunk
        return answer, rest[:size]


def pad_word(args):
    """The command prefix for the pad --pad names: "pad" for pad 1, "pad N" otherwise."""
    n = getattr(args, "pad", 1) or 1
    return "pad" if n == 1 else f"pad {n}"


def open_pad(args):
    """Create the pad, close the console's prompt, and (unless --beside) make the
    pad its user's first by disconnecting any real controller. The eviction runs
    in the payload, off the console's own kernel log, so it needs no klog server.
    Pads 2-4 (--pad) go to the next signed-in user without one and never disconnect
    a controller."""
    line = f"{pad_word(args)} open"
    if args.user:
        line += f" {args.user}"
    if args.beside:
        line += " keep"
    answer, _ = command(line)
    if not answer.startswith("ok") or "already open" in answer:
        return say(answer)
    print(answer)
    if not args.keep_prompt:
        # The console asks who the new controller belongs to although it is bound
        # already; Circle, from the pad itself, closes the question.
        time.sleep(1.5)
        command(f"{pad_word(args)} tap {BUTTONS['circle']:x} 80")
    return 0


def say(answer):
    print(answer)
    return 0 if answer.startswith("ok") else 1


def mask(names):
    value = 0
    for name in names.lower().replace(",", "+").split("+"):
        if name not in BUTTONS:
            raise SystemExit(f"unknown button {name!r}: {', '.join(BUTTONS)}")
        value |= BUTTONS[name]
    return value


def load(args):
    elf = Path(args.elf) if args.elf else ROOT / "build/ps5loop.elf"
    if not elf.is_file():
        raise SystemExit(f"missing payload: {elf} (run tools/build.sh)")
    with socket.create_connection((console_host(), LOADER_PORT), timeout=10) as connection:
        connection.sendall(elf.read_bytes())
    print(f"sent {elf.name} ({elf.stat().st_size} bytes) to the loader")
    if elf.name != "ps5loop.elf":
        return 0
    for _ in range(20):
        time.sleep(0.5)
        try:
            return say(command("ping", timeout=5)[0])
        except SystemExit:
            continue
    raise SystemExit("ps5loop: loaded, but the payload does not answer; read klog")


def run_tool(command):
    """Run a decode or convert helper. Returns the result, or None when the tool
    is not installed, so a capture is kept raw instead of crashing the client."""
    try:
        return subprocess.run(command, capture_output=True, text=True, check=False)
    except FileNotFoundError:
        return None


WIC_SCRIPT = """
Add-Type -AssemblyName PresentationCore
$in = [IO.File]::OpenRead($args[0])
$decoder = [Windows.Media.Imaging.BitmapDecoder]::Create($in, 'PreservePixelFormat', 'OnLoad')
$rgb = New-Object Windows.Media.Imaging.FormatConvertedBitmap($decoder.Frames[0], [Windows.Media.PixelFormats]::Bgr24, $null, 0)
$encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
$encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($rgb))
$out = [IO.File]::Create($args[1])
$encoder.Save($out)
$out.Close()
$in.Close()
"""


def wic_png(source, target):
    """Decode a JPEG XR with Windows' own codec (no jxrlib needed). True when the PNG was written."""
    if os.name != "nt":
        return False
    script = Path(target).with_suffix(".wic.ps1")
    script.write_text(WIC_SCRIPT, encoding="utf-8")
    try:
        done = run_tool(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                         str(Path(source).resolve()), str(Path(target).resolve())])
    finally:
        script.unlink(missing_ok=True)
    return done is not None and done.returncode == 0 and Path(target).is_file()


def pillow_scale(source, target):
    """Scale a picture to VIEW_WIDTH with Pillow, where ImageMagick is not installed."""
    try:
        from PIL import Image
    except ImportError:
        return False
    with Image.open(source) as picture:
        picture = picture.convert("RGB")
        if picture.width > VIEW_WIDTH:
            picture = picture.resize((VIEW_WIDTH, round(picture.height * VIEW_WIDTH / picture.width)))
        picture.save(target, "PNG")
    return True


def to_png(data, target):
    """Write a capture as a PNG to look at, whatever the console saved it as."""
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if data[:3] == b"II\xbc":
        # JPEG XR, what the console writes with HDR on.
        raw = target.with_suffix(".jxr")
        raw.write_bytes(data)
        tiff = target.with_suffix(".tif")
        # Asked for 24-bit RGB first: left to itself the decoder writes the home
        # screen's 10-bit pictures as a packed TIFF that nothing reads back
        # correctly. A title's 8-bit pictures refuse that conversion and decode
        # as they are.
        decoded = None
        for conversion in (["-c", "9"], []):
            tiff.unlink(missing_ok=True)
            decoded = run_tool(["JxrDecApp", "-i", str(raw), "-o", str(tiff), *conversion])
            if decoded is None or (decoded.returncode == 0 and tiff.is_file()):
                break
        if decoded is None:
            # No jxrlib: Windows decodes JPEG XR itself, and Pillow scales the result.
            if wic_png(raw, target) and pillow_scale(target, target):
                return target
            print(f"kept {raw}: install jxrlib (JxrDecApp) to decode the console's HDR captures")
            return raw
        if decoded.returncode != 0 or not tiff.is_file():
            print(f"kept {raw}: JxrDecApp could not decode it ({decoded.stdout.strip()[-200:]})")
            return raw
        source = tiff
    else:
        source = target.with_suffix(".png" if data[:4] == b"\x89PNG" else ".jpg")
        if source == target:
            source = target.with_suffix(".full.png")
        source.write_bytes(data)
    # Only the console's HDR pictures need their levels stretched to be readable.
    levels = ["-auto-level"] if source.suffix == ".tif" else []
    scaled = run_tool(["magick", str(source), *levels, "-resize", f"{VIEW_WIDTH}x>",
                       "-depth", "8", str(target)])
    if scaled is None:
        if pillow_scale(source, target):
            if source != target:
                source.unlink(missing_ok=True)
            return target
        print(f"kept {source}: install ImageMagick (magick) or Pillow to scale captures for viewing")
        return source
    if scaled.returncode != 0:
        print(f"kept {source}: magick could not convert it ({scaled.stderr.strip()[-200:]})")
        return source
    if source.suffix == ".tif":
        source.unlink()
    return target


def fetch(line, args):
    answer, data = command(line, timeout=60, payload=True)
    if not answer.startswith("ok file"):
        return say(answer)
    name = args.file or ROOT / "captures" / time.strftime("capture-%Y%m%d-%H%M%S.png")
    print(f"{answer.split(' path=')[0]} -> {to_png(data, name)}")
    return 0


def sequence(args):
    for step in args.steps:
        name, _, value = step.partition(":")
        if name == "wait":
            time.sleep(int(value or 500) / 1000)
            continue
        answer, _ = command(f"{pad_word(args)} tap {mask(name):x} {int(value or args.ms)}")
        if not answer.startswith("ok"):
            return say(answer)
        time.sleep(args.gap / 1000)
    print(f"ok {len(args.steps)} steps")
    return 0


def doctor():
    """Check the setup without pressing anything: the host, the payload and its
    loader, and the host tools captures need to decode. One line each, ok or --."""
    import shutil
    ok = True
    try:
        host = console_host()
    except SystemExit as error:
        print(error)
        return 1
    print("ok host set")
    try:
        answer, _ = command("ping", timeout=5)
        print(f"ok payload {answer}")
    except SystemExit as error:
        print(f"-- payload unreachable ({error}); run: ps5loop.py load")
        ok = False
    try:
        with socket.create_connection((host, LOADER_PORT), timeout=3):
            print(f"ok loader port {LOADER_PORT} open")
    except OSError:
        print(f"-- loader port {LOADER_PORT} closed (load needs it)")
        ok = False
    import importlib.util
    pillow = importlib.util.find_spec("PIL") is not None
    for tool, package in (("JxrDecApp", "jxrlib"), ("magick", "ImageMagick")):
        if shutil.which(tool):
            print(f"ok {tool} found")
        elif pillow and (tool == "magick" or os.name == "nt"):
            print(f"ok {tool} missing, covered by " + ("Pillow" if tool == "magick" else "Windows' own decoder"))
        else:
            print(f"-- {tool} missing: install {package} to decode captures "
                  "(raw files are still saved)")
            ok = False
    print("ok doctor: ready" if ok else "-- doctor: see the lines above")
    return 0 if ok else 1


def fetch_text(line):
    """Run a command whose answer is a text file (ps, ls) and print it."""
    answer, data = command(line, timeout=60, payload=True)
    if not answer.startswith("ok file"):
        return say(answer)
    sys.stdout.write(data.decode("utf-8", "replace"))
    return 0


def klog_stream(ms):
    """Stream the kernel log until the console closes it or you press Ctrl+C."""
    line = "klog-stream" + (f" {ms}" if ms else "")
    try:
        conn = socket.create_connection((console_host(), PORT), timeout=10)
    except OSError as error:
        raise SystemExit(f"ps5loop: the payload did not answer ({error})") from None
    with conn:
        conn.sendall((line + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = conn.recv(4096)
            if not chunk:
                return say("(no reply)")
            buf += chunk
        first, _, rest = buf.partition(b"\n")
        answer = first.decode("utf-8", "replace")
        if not answer.startswith("ok"):
            return say(answer)
        print(answer)
        sys.stdout.buffer.write(rest)
        sys.stdout.flush()
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                sys.stdout.buffer.write(chunk)
                sys.stdout.flush()
        except KeyboardInterrupt:
            pass
    return 0


def discover(timeout):
    """Broadcast a UDP probe and list the consoles that answer, on this LAN."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)
    sock.sendto(b"ps5loop?", ("255.255.255.255", PORT))
    found, end = [], time.time() + timeout
    while time.time() < end:
        try:
            data, addr = sock.recvfrom(256)
        except socket.timeout:
            break
        found.append((addr[0], data.decode("utf-8", "replace")))
    sock.close()
    for ip, info in found:
        print(f"{ip}  {info}")
    if not found:
        print("no consoles answered")
    return 0 if found else 1


def get_file(remote, local):
    """Download a console file to a local path (default: downloads/<name>)."""
    answer, data = command(f"get {remote}", timeout=300, payload=True)
    if not answer.startswith("ok file"):
        return say(answer)
    out = Path(local) if local else ROOT / "downloads" / Path(remote).name
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"ok get {len(data)} bytes -> {out}")
    return 0


def put_file(local, remote):
    """Upload a local file to a console path: one line, then the bytes."""
    data = Path(local).read_bytes()
    try:
        connection = socket.create_connection((console_host(), PORT), timeout=10)
    except OSError as error:
        raise SystemExit(f"ps5loop: the payload did not answer ({error.strerror or error})") from None
    with connection:
        connection.settimeout(300)
        connection.sendall(f"put {remote} {len(data)}\n".encode() + data)
        answer = b""
        while b"\n" not in answer:
            chunk = connection.recv(65536)
            if not chunk:
                break
            answer += chunk
    return say(answer.partition(b"\n")[0].decode("utf-8", "replace"))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("load").add_argument("elf", nargs="?")
    commands.add_parser("ping")
    commands.add_parser("raw").add_argument("words", nargs="+")
    opening = commands.add_parser("open")
    opening.add_argument("user", nargs="?", default="")
    opening.add_argument("--keep-prompt", action="store_true",
                         help="leave the console's \"Who's using this controller?\" prompt up")
    opening.add_argument("--beside", action="store_true",
                         help="leave a real controller connected (titles then read it, not the pad)")
    closing = commands.add_parser("close")
    closing.add_argument("--all", action="store_true", help="close every virtual pad")
    status = commands.add_parser("status")
    commands.add_parser("pads", help="every virtual pad's state (up to 4)")
    press = commands.add_parser("press")
    press.add_argument("buttons")
    press.add_argument("--ms", type=int, default=80)
    hold = commands.add_parser("hold")
    hold.add_argument("buttons", nargs="?", default="")
    for axis, default in (("lx", 128), ("ly", 128), ("rx", 128), ("ry", 128), ("l2", 0), ("r2", 0)):
        hold.add_argument(f"--{axis}", type=int, default=default)
    release = commands.add_parser("release")
    steps = commands.add_parser("seq")
    steps.add_argument("steps", nargs="+")
    steps.add_argument("--ms", type=int, default=80, help="how long each press is held")
    steps.add_argument("--gap", type=int, default=250, help="the pause after each press")
    for sub in (opening, closing, status, press, hold, release, steps):
        sub.add_argument("--pad", type=int, choices=range(1, 5), default=1,
                         help="which virtual pad, 1-4 (default 1)")
    screenshot = commands.add_parser("screenshot")
    screenshot.add_argument("file", nargs="?")
    screenshot.add_argument("--hold", type=int, default=0, help="how long Create is held, in ms")
    commands.add_parser("latest-capture").add_argument("file", nargs="?")
    commands.add_parser("list-captures")
    commands.add_parser("delete-capture").add_argument("path",
                                                       help="a capture under /user/av_contents/")
    kernel_log = commands.add_parser("kernel-log")
    kernel_log.add_argument("ms", nargs="?", default="1500", help="how long to read, in ms")
    kernel_log.add_argument("file", nargs="?")
    commands.add_parser("klog-stream").add_argument("ms", nargs="?",
                                                   help="optional cap in ms; default until you quit")
    commands.add_parser("doctor")
    commands.add_parser("discover").add_argument("--timeout", type=float, default=2.0)
    commands.add_parser("wake")
    commands.add_parser("ps")
    commands.add_parser("locate-title")
    commands.add_parser("close-app").add_argument("app_id", nargs="?",
                                                 help="an app id from ps; omitted, the running big app")
    commands.add_parser("kill").add_argument("pid")
    commands.add_parser("ls").add_argument("path")
    getting = commands.add_parser("get")
    getting.add_argument("remote")
    getting.add_argument("local", nargs="?")
    putting = commands.add_parser("put")
    putting.add_argument("local")
    putting.add_argument("remote")
    commands.add_parser("rm").add_argument("path")
    moving = commands.add_parser("mv")
    moving.add_argument("src")
    moving.add_argument("dst")
    commands.add_parser("df")
    commands.add_parser("hwinfo")
    args = parser.parse_args()

    if args.command == "load":
        return load(args)
    if args.command == "open":
        return open_pad(args)
    if args.command == "seq":
        return sequence(args)
    if args.command == "screenshot":
        return fetch(f"screenshot {args.hold}" if args.hold else "screenshot", args)
    if args.command == "latest-capture":
        return fetch("latest-capture", args)
    if args.command == "kernel-log":
        answer, data = command(f"kernel-log {args.ms}", timeout=60, payload=True)
        if not answer.startswith("ok file"):
            return say(answer)
        out = Path(args.file) if args.file else ROOT / "captures" / time.strftime("klog-%Y%m%d-%H%M%S.txt")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        print(f"ok kernel-log {len(data)} bytes -> {out}")
        return 0
    if args.command == "klog-stream":
        return klog_stream(args.ms)
    if args.command == "doctor":
        return doctor()
    if args.command == "discover":
        return discover(args.timeout)
    if args.command == "wake":
        return wake()
    if args.command in ("ps", "df"):
        return fetch_text(args.command)
    if args.command == "ls":
        return fetch_text(f"ls {args.path}")
    if args.command == "get":
        return get_file(args.remote, args.local)
    if args.command == "put":
        return put_file(args.local, args.remote)
    if args.command == "press":
        return say(command(f"{pad_word(args)} tap {mask(args.buttons):x} {args.ms}")[0])
    if args.command == "hold":
        buttons = mask(args.buttons) if args.buttons else 0
        return say(command(f"{pad_word(args)} set {buttons:x} {args.lx} {args.ly} {args.rx} {args.ry} "
                           f"{args.l2} {args.r2}")[0])
    line = {"ping": "ping", "list-captures": "list-captures", "status": f"{pad_word(args)} status",
            "close": "pad close all" if getattr(args, "all", False) else f"{pad_word(args)} close",
            "pads": "pad list", "raw": " ".join(getattr(args, "words", [])),
            "release": f"{pad_word(args)} set 0 128 128 128 128 0 0",
            "delete-capture": f"delete-capture {getattr(args, 'path', '')}",
            "rm": f"rm {getattr(args, 'path', '')}",
            "mv": f"mv {getattr(args, 'src', '')} {getattr(args, 'dst', '')}",
            "close-app": f"close-app {getattr(args, 'app_id', None) or ''}".strip(),
            "kill": f"kill {getattr(args, 'pid', '')}",
            "locate-title": "locate-title", "hwinfo": "hwinfo"}[args.command]
    return say(command(line)[0])


if __name__ == "__main__":
    sys.exit(main())
