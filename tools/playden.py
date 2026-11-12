#!/usr/bin/env python3
"""Test Playden games for compatibility, driven by an agent through DevLoop.

DevLoop (ps5loop.py) gives the pad and the screen. This adds what a compatibility
test needs around them: which game is running, the timed watch, Playden's logs
(redacted), and a report folder with a draft for Playden's test-report form.
The agent looks at the pictures and plays; this keeps the record.

  playden.py status                    what answers, what runs, free space
  playden.py games                     the games Playden has prepared (app, title, state)
  playden.py begin APP "NAME"          start a report: reports/<app>-<name>/, now the current one
  playden.py launch                    start the current game's PS5 title (Playden's relaunch helper)
  playden.py watch                     90 s: is it running, pictures at 30 s and 90 s, black/still/moving
  playden.py shot LABEL [--note TEXT]  a picture into the report, as a numbered step
  playden.py note TEXT                 a step without a picture ("Cross on New Game: intro plays")
  playden.py close                     close the game, wait until it is gone
  playden.py logs                      Playden's logs into the report, redacted; fatal lines and the tail
  playden.py verdict RESULT --summary TEXT --model NAME   result.json and issue.md (a draft, never posted)
  playden.py summary                   reports/summary.md: every report on one page

RESULT is works, broken, no-start, refused or untested. AGENTS.md says how to
reach one. The console's address comes from PS5_HOST or .env, as for ps5loop.py.
"""
import argparse
import json
import re
import socket
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ps5loop  # noqa: E402

ROOT = ps5loop.ROOT
REPORTS = ROOT / "reports"
CURRENT = REPORTS / "current.txt"

PLAYDEN = "FAKE77031"
HOMEBREW = "/data/homebrew"
LOGS = "/data/playden/logs"
KITS = "/data/playden/kits"
GAMES = "/data/playden-games"
SANDBOX = "/mnt/sandbox"
PRIVATE = "/data/playden/user"  # session.v1, the Steam sign-in, lives here: never read
RELAUNCH_ELF = f"{HOMEBREW}/{PLAYDEN}/self-update-relaunch.elf"
REQUEST = "/data/playden/self-update/relaunch-title"
RESULT = "/data/playden/self-update/relaunch.log"

TITLE_ID = re.compile(r"^[A-Z]{4}\d{5}$")
START_TIMEOUT = 45.0   # seconds for the game to appear after a launch
BLACK_BELOW = 10.0     # mean brightness, of 255
STILL_BELOW = 0.6      # mean change per pixel between two pictures

RESULTS = {
    "works": "✅ Works: reaches the menu and plays",
    "broken": "❌ Broken: starts, but is not playable",
    "no-start": "❌ Does not start: back to Home, or stays on the spinner",
    "refused": "⛔ Refused: Playden would not prepare it (say below whether Try anyway or another Program helped)",
    "untested": "",  # the tester went wrong: no draft, since it says nothing about the game
}

ACCOUNT = re.compile(r'L"Steam(?:App)?User" L"([^"]+)"')
STEAM_ID = re.compile(r"\b7656119\d{10}\b")
FIRMWARE = re.compile(r"console_firmware=(\S+)")
HEADER = re.compile(r"^== (session-\d+\.log)", re.MULTILINE)
REPORT = re.compile(r"^PW_REPORT/1 .*?\bprofile=(\S+).*?\btime=(\d+)", re.MULTILINE)
ENDED = re.compile(r"^(session-\d+\.log) bytes=(\d+) time=(\d+)", re.MULTILINE)
WINE_ERR = re.compile(r"WINE [0-9a-f]{4}:err:(\w+)")
RC = re.compile(r"(sceLncUtilLaunchApp|sceSystemServiceLaunchApp) rc=0x([0-9a-f]{8})")
FATAL = [
    (re.compile(r"Crash!!!"), "crash"),
    (re.compile(r"[Uu]nhandled exception"), "unhandled exception"),
    (re.compile(r"Assertion .* failed|assertion failed"), "assertion"),
    (re.compile(r"\bwine: could not load\b|err:module:import_dll"), "missing dll"),
    (re.compile(r"\bstatus=(?!0\b|ok\b)(-?\w+)\b.*\bstage="), "load stage failed"),
]


class ConsoleError(Exception):
    pass


# --- the console, through the DevLoop payload ---------------------------------------------------------------------

def allowed(path):
    """Refuse the folder that holds the Steam sign-in, on every route."""
    clean = re.sub(r"/+", "/", path)
    if clean == PRIVATE or clean.startswith(PRIVATE + "/") or "session.v1" in clean:
        raise ConsoleError(f"{path} is off limits: it holds the Steam sign-in")
    return path


def ask(line, timeout=30.0):
    answer, _ = ps5loop.command(line, timeout=timeout)
    if not answer.startswith("ok"):
        raise ConsoleError(answer)
    return answer


def fetch(line, timeout=120.0):
    answer, data = ps5loop.command(line, timeout=timeout, payload=True)
    if not answer.startswith("ok file"):
        raise ConsoleError(answer)
    return data


def read_text(path):
    return fetch(f"get {allowed(path)}").decode("utf-8", "replace")


def parse_ls(text):
    """ls lines are "<type> <size> <name>": [(type, size, name)]."""
    rows = []
    for line in text.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1].isdigit():
            rows.append((parts[0], int(parts[1]), parts[2]))
    return rows


def ls(path):
    return parse_ls(fetch(f"ls {allowed(path)}").decode("utf-8", "replace"))


def parse_ps(text):
    """ps rows as dicts (pid, ppid, app_id, title, command); app_id and title are "" for plain processes."""
    rows = []
    for line in text.splitlines()[1:]:
        parts = line.split(None, 4)
        if len(parts) == 5 and parts[0].isdigit():
            rows.append({"pid": int(parts[0]), "ppid": int(parts[1]), "app_id": parts[2].strip("-"),
                         "title": parts[3].strip("-"), "command": parts[4]})
    return rows


def processes():
    return parse_ps(fetch("ps").decode("utf-8", "replace"))


def title_digits(rows):
    """The number of every title with a live process. On firmware 12.20 ps prints only the number ("78024" for
    FAKE78024), so the number is what processes are matched by."""
    return {row["title"][-5:] for row in rows if re.fullmatch(r"(?:[A-Z]{4})?\d{5}", row["title"])}


def running_titles(rows=None, sandbox=None):
    """Title IDs that have both a sandbox folder and a live process. The folder gives the full ID; the process
    proves it: Playden and some games leave their folder behind after they quit."""
    digits = title_digits(processes() if rows is None else rows)
    names = {name.partition("_")[0] for _, _, name in (ls(SANDBOX) if sandbox is None else sandbox)}
    return {name for name in names if TITLE_ID.match(name) and name[4:] in digits}


def is_running(title):
    return title in running_titles()


def port_open(port, timeout=1.5):
    try:
        with socket.create_connection((ps5loop.console_host(), port), timeout=timeout):
            return True
    except OSError:
        return False


# --- Playden's logs -----------------------------------------------------------------------------------------------

def runtime_log():
    """runtime.log.1 then runtime.log, oldest first; "" when Playden has not written one."""
    text = ""
    for name in ("runtime.log.1", "runtime.log"):
        try:
            text += read_text(f"{LOGS}/{name}")
        except ConsoleError:
            pass
    return text


def reserved_title(log, app):
    """The PS5 title Playden last reserved for an app ("prepare reserved app=<app> title=FAKE78024"), or None."""
    found = [title for number, title in re.findall(r" prepare reserved app=(\w+) title=(\w+)", log)
             if number == str(app)]
    return found[-1] if found else None


def prepare_state(log, app):
    """(None, "") with no prepare of the app in the log, ("ready", line), ("failed", line) or ("preparing", line)."""
    lines = [l for l in log.splitlines() if " prepare " in l and re.search(rf"\bapp={app}\b", l)]
    if not lines:
        return None, ""
    newest = lines[-1]
    if " prepare result " in newest:
        return ("ready" if newest.rstrip().endswith("ready") else "failed"), newest
    return "preparing", newest


def prepared_games(log):
    """{app: (title, state)} for every app the log shows Playden preparing."""
    games = {}
    for app in dict.fromkeys(re.findall(r" prepare \w+ app=(\d+)", log)):
        games[app] = (reserved_title(log, app), prepare_state(log, app)[0])
    return games


def sessions(digest):
    """The sessions of a title-<ID>.log digest, newest first: name, start, end, seconds, text, problems, wine_errors."""
    ended = {name: int(stamp) for name, _, stamp in ENDED.findall(digest)}
    marks = list(HEADER.finditer(digest))
    reports_at = digest.find("\n== reports")
    found = []
    for i, mark in enumerate(marks):
        stop = marks[i + 1].start() if i + 1 < len(marks) else len(digest)
        if mark.start() < reports_at < stop:
            stop = reports_at
        body = digest[mark.start():stop]
        report = REPORT.search(body)
        if not report:
            continue
        start, end = int(report.group(2)), ended.get(mark.group(1), 0)
        problems, errors, seen = [], {}, set()
        for line in body.splitlines():
            for pattern, label in FATAL:
                if label not in seen and pattern.search(line):
                    seen.add(label)
                    problems.append(f"{label}: {line.strip()[:300]}")
            hit = WINE_ERR.search(line)
            if hit:
                errors[hit.group(1)] = errors.get(hit.group(1), 0) + 1
        found.append({"name": mark.group(1), "start": start, "end": end, "seconds": max(end - start, 0),
                      "text": body, "problems": problems, "wine_errors": errors})
    return sorted(found, key=lambda s: s["start"], reverse=True)


def alive_stats(text):
    """The counters of the last "alive" line of a session (vk_shown, audible, frames_put, ...)."""
    lines = [l for l in text.splitlines() if " alive " in l]
    return {k: int(v) for k, v in re.findall(r"\b(\w+)=(\d+)\b", lines[-1])} if lines else {}


def redact(text, names):
    text = STEAM_ID.sub("<steam-id>", text)
    for name in sorted(names, key=len, reverse=True):
        text = re.sub(re.escape(name), "<steam-user>", text, flags=re.IGNORECASE)
    return text


def account_names(texts):
    return {match.group(1) for text in texts for match in ACCOUNT.finditer(text)}


def launch_outcome(text):
    """From the relaunch helper's log: True started, False failed, None not finished yet."""
    if "bad title id" in text or "no request" in text:
        return False
    wait = re.search(r"wait state=(-?\d+)", text)
    if wait and wait.group(1) != "0":
        return False
    codes = [int(code, 16) for _, code in RC.findall(text)]
    if codes and not codes[-1] & 0x80000000:
        return True
    if len(codes) >= 2:
        return False
    return None


# --- the report folder --------------------------------------------------------------------------------------------

def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "game"


def current():
    """The report folder `begin` made current, and its run.json."""
    if not CURRENT.is_file():
        raise SystemExit("no current report: run  playden.py begin APP \"NAME\"  first")
    folder = REPORTS / CURRENT.read_text(encoding="utf-8").strip()
    return folder, json.loads((folder / "run.json").read_text(encoding="utf-8"))


def save(folder, run):
    (folder / "run.json").write_text(json.dumps(run, indent=2, ensure_ascii=False) + "\n", encoding="utf-8",
                                     newline="\n")


def stamp():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def need_title(run):
    if not run.get("title"):
        raise SystemExit("this report has no PS5 title yet: Playden reserves one when it prepares the game; "
                         "run  playden.py begin APP NAME  again once  playden.py games  lists it, or pass --title")
    return run["title"]


def screenshot(target):
    """The whole screen through the console's screenshot (the pad must be open). The path written."""
    answer, data = ps5loop.command("screenshot", timeout=60, payload=True)
    if not answer.startswith("ok file"):
        raise ConsoleError(f"{answer} (is the pad open?  ps5loop.py open)")
    return Path(ps5loop.to_png(data, target))


def measure(first, second=None):
    """Brightness of a picture (0-255) and, with a second one, the mean change per pixel between them. {} without
    Pillow or a readable picture: then look at the pictures instead."""
    try:
        from PIL import Image, ImageChops, ImageStat
        with Image.open(first) as a:
            a = a.convert("L").resize((320, 180))
        screen = {"brightness": round(ImageStat.Stat(a).mean[0], 1)}
        if second:
            with Image.open(second) as b:
                b = b.convert("L").resize((320, 180))
            screen["motion"] = round(ImageStat.Stat(ImageChops.difference(a, b)).mean[0], 2)
    except (ImportError, OSError):
        return {}
    if screen["brightness"] < BLACK_BELOW:
        screen["verdict"] = "black"
    elif "motion" in screen:
        screen["verdict"] = "still" if screen["motion"] < STILL_BELOW else "moving"
    return screen


# --- commands -----------------------------------------------------------------------------------------------------

def cmd_status(args):
    try:
        print(f"ok payload: {ps5loop.command('ping', timeout=5)[0]}")
    except SystemExit as error:
        print(f"-- {error}")
        return 1
    print(f"{'ok' if port_open(ps5loop.LOADER_PORT) else '--'} ELF loader port {ps5loop.LOADER_PORT} "
          "(launch needs it)")
    rows = processes()
    apps = sorted(title for title in running_titles(rows) if not title.startswith("NPXS"))  # NPXS: the system's own
    for title in apps:
        what = "Playden" if title == PLAYDEN else "a Playden game" if title.startswith("FAKE78") else "a title"
        row = next(row for row in rows if row["title"][-5:] == title[4:])
        print(f"   running: {title} ({what}) app {row['app_id'] or '-'} pid {row['pid']} {row['command']}")
    if not apps:
        print("   running: no game or app, only the system")
    print(ps5loop.command("pad status")[0])
    try:
        lines = fetch("df").decode("utf-8", "replace").splitlines()
        for line in lines:  # the game volumes only: every sandbox has a dozen mounts of its own
            if line.split()[:1] and line.split()[0] in ("MOUNT", "/user", "/data") or line.startswith(("/mnt/ext", "/mnt/usb")):
                print(line)
    except ConsoleError as error:
        print(f"-- df: {error}")
    if CURRENT.is_file():
        folder, run = current()
        print(f"current report: {folder.name} (title {run.get('title') or 'not reserved yet'})")
    return 0


def title_holders(log):
    """{title: app} for the app that reserved each PS5 title last. Playden hands a title to the next game once its
    game is uninstalled, so an older app's title is no longer its own."""
    return {title: app for app, title in re.findall(r" prepare reserved app=(\w+) title=(\w+)", log)}


def cmd_games(args):
    log = runtime_log()
    games, holders = prepared_games(log), title_holders(log)
    try:
        installed = [name for kind, _, name in ls(GAMES) if kind == "d" and not name.startswith(".")]
    except ConsoleError:
        installed = []
    live = running_titles()
    print(f"{'APP':>12}  {'TITLE':<10} {'STATE':<12} RUNNING")
    for app in sorted(installed, key=lambda name: (not name.isdigit(), int(name) if name.isdigit() else 0, name)):
        title, state = games.get(app, (None, None))
        if title and holders.get(title) != app:
            title, state = None, "title reused"  # prepared once, but its title went to a later game
        short = app if len(app) <= 12 else app[:11] + "~"
        print(f"{short:>12}  {title or '-':<10} {state or 'not prepared':<12} {'yes' if title in live else ''}")
    if not installed:
        print("No game is installed: install one from Playden's Library first")
    print(f"{len(installed)} installed (the folders of {GAMES}). A game with no title is prepared by Playden when "
          "you open its page and press Play.")
    return 0


def cmd_begin(args):
    title = args.title
    if not title:
        try:
            title = reserved_title(runtime_log(), args.app)
        except SystemExit:
            title = None  # no console yet: the title can be filled in by a later begin
    if title and not TITLE_ID.match(title):
        raise SystemExit(f"not a title ID: {title!r}")
    folder = REPORTS / f"{args.app}-{slug(args.name)}"
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / "run.json"
    run = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {"steps": []}
    run.update({"app": args.app, "name": args.name, "title": title or run.get("title"),
                "started_at": run.get("started_at") or stamp()})
    save(folder, run)
    CURRENT.write_text(folder.name + "\n", encoding="utf-8", newline="\n")
    print(f"ok report {folder} (title {run['title'] or 'not reserved yet: is the game installed and prepared?'})")
    return 0


def cmd_launch(args):
    folder, run = current()
    title = args.title or need_title(run)
    state, line = prepare_state(runtime_log(), run["app"])
    if state not in ("ready", None) and not args.force:
        raise SystemExit(f"Playden has not finished preparing app {run['app']} ({state}): {line.strip()[-160:]}\n"
                         "A game started now quits at once, which says nothing about it. Wait, or --force.")
    if not port_open(ps5loop.LOADER_PORT):
        raise SystemExit(f"the ELF loader (port {ps5loop.LOADER_PORT}) is not up: start the game from Playden "
                         "with the pad instead (its page, Play)")
    helper = ROOT / "downloads" / "self-update-relaunch.elf"
    helper.parent.mkdir(parents=True, exist_ok=True)
    helper.write_bytes(fetch(f"get {RELAUNCH_ELF}"))
    request = folder / "relaunch-title.txt"
    request.write_text(title + "\n", encoding="utf-8", newline="\n")
    ps5loop.command(f"rm {RESULT}")  # an old answer would be read as this launch's
    if ps5loop.put_file(request, REQUEST) != 0:
        raise SystemExit("could not write the launch request")
    request.unlink()
    with socket.create_connection((ps5loop.console_host(), ps5loop.LOADER_PORT), timeout=10) as loader:
        loader.sendall(helper.read_bytes())
    deadline, outcome, text = time.monotonic() + 20, None, ""
    while outcome is None and time.monotonic() < deadline:
        time.sleep(1.0)
        try:
            text = read_text(RESULT)
        except ConsoleError:
            continue
        if f"title={title}" in text:
            outcome = launch_outcome(text)
    # A failed return code does not settle it: with Playden in front both launch calls answer 0x80940010 and the
    # game starts anyway. Only a game that never appears counts as a failed launch.
    deadline = time.monotonic() + 15
    while not is_running(title) and time.monotonic() < deadline:
        time.sleep(1.0)
    started = is_running(title)
    run["launch"] = {"at": stamp(), "helper": outcome, "started": started}
    run["steps"].append({"at": stamp(), "text": "Started from its PS5 title (Playden's relaunch helper)."})
    save(folder, run)
    if started:
        print(f"ok {title} is running; now  playden.py watch")
        return 0
    codes = ", ".join("0x" + code for _, code in RC.findall(text)) or "no answer"
    print(f"-- {title} did not start ({codes}). Take a screenshot; start it from Playden with the pad if the "
          "screen shows no error")
    return 1


def cmd_watch(args):
    folder, run = current()
    title = need_title(run)
    began = time.monotonic()
    while not is_running(title):
        if time.monotonic() - began > START_TIMEOUT:
            run["watch"] = {"status": "did_not_start", "seconds_running": 0, "at": stamp()}
            save(folder, run)
            print(f"did_not_start: {title} did not appear within {START_TIMEOUT:.0f} s; take a screenshot")
            return 1
        time.sleep(2.0)
    start = time.monotonic()
    watch = {"status": "", "at": stamp(), "seconds": args.seconds, "screenshots": []}
    print(f"{title} is running; watching for {args.seconds:.0f} s, nothing pressed")

    def wait_until(seconds):
        """Sleep until `seconds` into the watch; False when the game quit first."""
        while time.monotonic() - start < seconds:
            time.sleep(2.0)
            if not is_running(title):
                return False
        return True

    alive = True
    if 0 < args.first < args.seconds:
        alive = wait_until(args.first)
        if alive:
            shot = screenshot(folder / f"screenshot-{args.first:.0f}s.png")
            watch["screenshots"].append(shot.name)
            print(f"  {args.first:.0f} s: {shot}")
    alive = alive and wait_until(args.seconds)
    if not alive:
        watch.update(status="exited_early", seconds_running=round(time.monotonic() - start))
        try:
            shot = screenshot(folder / "screenshot-after-exit.png")
            watch["screenshots"].append(shot.name)
        except (ConsoleError, SystemExit) as error:
            print(f"  no picture after it quit: {error}")
        run["watch"] = watch
        save(folder, run)
        print(f"exited_early after {watch['seconds_running']} s. Look at the picture, then  playden.py logs  "
              "(a fatal line there makes it a crash)")
        return 1
    end = f"{args.seconds:.0f}s"
    first = screenshot(folder / f"screenshot-{end}.png")
    second = screenshot(folder / f"screenshot-{end}-b.png")
    watch["screenshots"] += [first.name, second.name]
    watch.update(status="survived", seconds_running=round(time.monotonic() - start), screen=measure(first, second))
    run["watch"] = watch
    save(folder, run)
    screen = watch["screen"]
    print(f"survived {watch['seconds_running']} s; screen {screen.get('verdict', 'not measured')} "
          f"(brightness {screen.get('brightness', '?')}/255, change {screen.get('motion', '?')} between "
          f"{first.name} and {second.name})")
    print("Look at the pictures: the numbers are a hint. Then play it (AGENTS.md, \"Play it\").")
    return 0


def cmd_shot(args):
    folder, run = current()
    number = 1 + sum(1 for step in run["steps"] if step.get("picture"))
    shot = screenshot(folder / f"step-{number:02d}-{slug(args.label)}.png")
    run["steps"].append({"at": stamp(), "text": args.note or args.label, "picture": shot.name})
    save(folder, run)
    print(f"ok {shot}")
    return 0


def cmd_note(args):
    folder, run = current()
    run["steps"].append({"at": stamp(), "text": args.text})
    save(folder, run)
    print(f"ok step {len(run['steps'])}")
    return 0


def cmd_close(args):
    folder, run = current()
    title = need_title(run)
    apps = [row for row in processes() if row["title"][-5:] == title[4:] and row["app_id"]]
    if not apps:
        print(f"ok {title} is not running")
        return 0
    print(ask(f"close-app {apps[0]['app_id']}"))  # by id: a bare close-app closes whatever is in front
    deadline = time.monotonic() + 20
    while is_running(title):
        if time.monotonic() > deadline:
            print(f"-- {title} still runs 20 s after it was closed")
            return 1
        time.sleep(1.0)
    run["closed_at"] = stamp()
    save(folder, run)
    print(f"ok {title} closed. Bring Playden back before  playden.py logs : it copies the game's log when it starts")
    return 0


def cmd_logs(args):
    folder, run = current()
    title = need_title(run)
    texts = {}
    for kind, _, name in ls(LOGS):
        if kind != "-" or name.endswith(".ppm"):
            continue
        if name.startswith("title-") and not name.startswith(f"title-{title}."):
            continue  # other games' logs stay out of this game's report
        try:
            texts[name] = read_text(f"{LOGS}/{name}")
        except ConsoleError as error:
            print(f"   skipped {name}: {error}")
    names = account_names(texts.values())
    texts = {name: redact(text, names) for name, text in texts.items()}
    info = {"collected": time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime())}
    for label, path in (("Playden version", f"{HOMEBREW}/{PLAYDEN}/playden-version"),
                        ("Playden package", f"{HOMEBREW}/{PLAYDEN}/playden-package.identity")):
        try:
            words = read_text(path).split()
            info[label] = (words[-1][:12] if "package" in label else " ".join(words)) if words else "unknown"
        except ConsoleError:
            info[label] = "unknown"
    firmware = [m.group(1) for name in ("runtime.log.1", "runtime.log") for m in FIRMWARE.finditer(texts.get(name, ""))]
    info["firmware"] = firmware[-1] if firmware else "unknown"
    info["kit runtime"] = kit_runtime()
    info["game title"] = f"{title} (app {run['app']})"
    info["redacted"] = "Steam account name and SteamIDs"
    table = "\n".join(["| Field | Value |", "| --- | --- |"] + [f"| {k} | {v} |" for k, v in info.items()]) + "\n"
    out = folder / f"{folder.name}-logs.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("summary.md", table)
        for name, text in sorted(texts.items()):
            archive.writestr(f"logs/{name}", text)
    found = sessions(texts.get(f"title-{title}.log", ""))
    newest = found[0] if found else None
    stale = bool(newest) and newest["start"] == run.get("logs", {}).get("session_start")
    run["versions"] = table
    run["logs"] = {"zip": out.name, "at": stamp()}
    if newest:
        tail = "\n".join(newest["text"].rstrip().splitlines()[-args.tail:])
        run["logs"].update(session=newest["name"], session_start=newest["start"], session_seconds=newest["seconds"],
                           problems=newest["problems"], wine_errors=newest["wine_errors"],
                           alive=alive_stats(newest["text"]), tail=tail)
    save(folder, run)
    print(f"ok {out} ({len(texts)} logs, redacted)")
    print(table)
    if not newest:
        print(f"No session in title-{title}.log yet. Playden copies a game's log when Playden next starts: close "
              "the game, bring Playden back (its Home tile), then run this again.")
        return 1
    if stale:
        print("-- This is the same session as the last time logs ran: Playden has not copied the new one yet.")
    print(f"newest session {newest['name']}: {newest['seconds']} s")
    for problem in newest["problems"]:
        print(f"  FATAL {problem}")
    if not newest["problems"]:
        print("  no fatal line")
    if newest["wine_errors"]:
        print("  wine err: " + ", ".join(f"{k} x{v}" for k, v in sorted(newest["wine_errors"].items(),
                                                                         key=lambda kv: -kv[1])))
    if run["logs"]["alive"]:
        print(f"  last alive counters: {run['logs']['alive']}")
    print("--- end of the session ---")
    print(run["logs"]["tail"])
    return 0


def kit_runtime():
    try:
        kits = sorted((name for kind, _, name in ls(KITS) if kind == "d" and not name.startswith(".")),
                      key=lambda name: name != "playden-kit")
    except ConsoleError:
        return "unknown"
    for name in kits:
        try:
            manifest = json.loads(read_text(f"{KITS}/{name}/manifest.json"))
        except (ConsoleError, ValueError):
            continue
        return (manifest.get("runtime") or {}).get("artifact_revision") or "unknown"
    return "unknown"


def issue_draft(run, folder_name):
    """The draft for Playden's test-report form, or None for a run that says nothing about the game."""
    status = RESULTS[run["result"]]
    if not status:
        return None
    logs, watch = run.get("logs", {}), run.get("watch", {})
    store = f"https://store.steampowered.com/app/{run['app']}" if str(run["app"]).isdigit() else "(imported)"
    steps = [step["text"] + (f" (`{step['picture']}`)" if step.get("picture") else "") for step in run["steps"]]
    if watch:
        screen = watch.get("screen", {})
        steps.insert(1 if run.get("launch") else 0,
                     f"Watched for {watch.get('seconds_running', 0)} s with nothing pressed: {watch['status']}"
                     + (f", screen {screen['verdict']} (brightness {screen.get('brightness')}/255, change "
                        f"{screen.get('motion')})" if screen.get("verdict") else "") + ".")
    if logs.get("session"):
        steps.append(f"Session in the log: {logs['session']} ({logs.get('session_seconds', 0)} s).")
    pictures = watch.get("screenshots", []) + [step["picture"] for step in run["steps"] if step.get("picture")]
    lines = [
        f"<!-- Draft from ps5-auto-test, {run.get('verdict_at', '')}. Not posted. Title: [Test] {run['name']} -->",
        f"# [Test] {run['name']}", "",
        "### Game", "", f"{run['name']} — {store}", "",
        "### Result", "", status, "",
        "### Versions", "", (run.get("versions") or "(run  playden.py logs  for the Versions table)").strip(), "",
        f"This report is AI generated using {run['model']}.", "",
        "### Where the game is installed", "", run.get("storage", "Console storage"), "",
        "### Controls", "", f"- [x] {run.get('controls')}", "",
        "### Game settings you changed", "", run.get("settings", "None."), "",
        "### What happened", "", run["summary"], "",
        *[f"{number}. {text}" for number, text in enumerate(steps, 1)], "",
        "### Logs and screenshots", "",
        (f"`{logs['zip']}` (attach it)" if logs.get("zip") else "No logs collected")
        + "".join(f", `{name}`" for name in pictures) + ".", "",
    ]
    if logs.get("problems"):
        lines += ["Fatal lines found:", ""] + [f"- `{problem}`" for problem in logs["problems"]] + [""]
    if logs.get("tail"):
        lines += [f"From `logs/title-{run['title']}.log`, near the end:", "", "```",
                  *logs["tail"].splitlines()[-15:], "```", ""]
    lines += ["### Checks", "", "- [ ] kstuff and ShadowMountPlus were loaded when I tested.",
              "- [ ] I own this game (on Steam, or a copy I bought for an imported game).", ""]
    return "\n".join(lines)


def cmd_verdict(args):
    folder, run = current()
    run.update(result=args.result, summary=args.summary, model=args.model, verdict_at=stamp(),
               controls=args.controls, played=args.played)
    if args.settings:
        run["settings"] = args.settings
    if args.storage:
        run["storage"] = args.storage
    save(folder, run)
    watch, logs = run.get("watch", {}), run.get("logs", {})
    result = {"app": run["app"], "name": run["name"], "title": run.get("title"), "result": args.result,
              "summary": args.summary, "played": args.played, "watch": watch.get("status", ""),
              "screen": watch.get("screen", {}).get("verdict", ""), "problems": logs.get("problems", []),
              "session_seconds": logs.get("session_seconds"), "tested_at": run["verdict_at"], "model": args.model}
    (folder / "result.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8",
                                        newline="\n")
    draft = issue_draft(run, folder.name)
    if draft is None:
        (folder / "issue.md").unlink(missing_ok=True)
        print(f"ok {folder / 'result.json'} (untested: no draft, the run says nothing about the game)")
        return 0
    (folder / "issue.md").write_text(draft, encoding="utf-8", newline="\n")
    print(f"ok {folder / 'issue.md'} ({RESULTS[args.result]})")
    print("A draft only: a person reviews it and ticks the Checks before anything is posted.")
    if not logs.get("zip"):
        print("-- no logs in this report yet: run  playden.py logs  and  verdict  again")
    return 0


def cmd_summary(args):
    rows = []
    for path in sorted(REPORTS.glob("*/result.json")):
        result = json.loads(path.read_text(encoding="utf-8"))
        rows.append(f"| {result['name']} | {result['app']} | {result['result']} | {result.get('played') or '-'} | "
                    f"{result['summary']} | {result['tested_at'][:10]} | [{path.parent.name}]({path.parent.name}/) |")
    lines = ["# Playden compatibility tests", "", f"{len(rows)} games.", "",
             "| Game | App | Result | Played | What happened | Tested | Report |",
             "| --- | --- | --- | --- | --- | --- | --- |", *rows, ""]
    REPORTS.mkdir(exist_ok=True)
    (REPORTS / "summary.md").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"ok {REPORTS / 'summary.md'} ({len(rows)} games)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status").set_defaults(run=cmd_status)
    commands.add_parser("games").set_defaults(run=cmd_games)
    begin = commands.add_parser("begin")
    begin.add_argument("app", help="the Steam app number, or a short name for an imported program")
    begin.add_argument("name", help="the game's name as its store page has it")
    begin.add_argument("--title", help="its PS5 title (FAKE78xxx), when Playden's log does not name it")
    begin.set_defaults(run=cmd_begin)
    launch = commands.add_parser("launch")
    launch.add_argument("--title")
    launch.add_argument("--force", action="store_true", help="start it although Playden is still preparing it")
    launch.set_defaults(run=cmd_launch)
    watch = commands.add_parser("watch")
    watch.add_argument("--seconds", type=float, default=90.0)
    watch.add_argument("--first", type=float, default=30.0, help="when the first picture is taken (0: none)")
    watch.set_defaults(run=cmd_watch)
    shot = commands.add_parser("shot")
    shot.add_argument("label", help="a few words: main-menu, new-game, in-game")
    shot.add_argument("--note", help="what you pressed and what the picture shows")
    shot.set_defaults(run=cmd_shot)
    commands.add_parser("note").add_argument("text")
    commands.choices["note"].set_defaults(run=cmd_note)
    commands.add_parser("close").set_defaults(run=cmd_close)
    logs = commands.add_parser("logs")
    logs.add_argument("--tail", type=int, default=30, help="lines of the session's end to print and keep")
    logs.set_defaults(run=cmd_logs)
    verdict = commands.add_parser("verdict")
    verdict.add_argument("result", choices=sorted(RESULTS))
    verdict.add_argument("--summary", required=True, help="one or two sentences: how far it got and what went wrong")
    verdict.add_argument("--model", required=True, help="the model that tested it, named in the draft")
    verdict.add_argument("--played", choices=["no", "menu", "gameplay"], default="no",
                         help="how far the pad got: nothing pressed, menus only, or into the game itself")
    verdict.add_argument("--controls", default="DualSense, default (Xbox controller) (DevLoop's virtual DualSense)")
    verdict.add_argument("--settings", help="game settings changed in Playden (default: None.)")
    verdict.add_argument("--storage", help="where it is installed (default: Console storage)")
    verdict.set_defaults(run=cmd_verdict)
    commands.add_parser("summary").set_defaults(run=cmd_summary)
    args = parser.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")  # a Windows console would refuse the form's marks
    try:
        return args.run(args)
    except ConsoleError as error:
        print(f"-- {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
