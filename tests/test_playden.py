"""playden.py without a console: its parsing, redaction, screen measure, the watch and the drafts."""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import playden  # noqa: E402

PS = """   PID   PPID APPID      TITLEID      COMMAND
     1      0 -          -            init
    88      1 0x7        40087        SceShellUI
   140     88 0x60000031 77031        eboot.bin
   151     88 0x60000032 FAKE78024    eboot.bin
"""
SANDBOX = [("d", 0, "NPXS40087_000"), ("d", 0, "FAKE77031_000"), ("d", 0, "FAKE78024_000"),
           ("d", 0, "FAKE78030_000")]  # 78030 quit and left its folder
RUNTIME = """12:00:01 prepare reserved app=620 title=FAKE78024
12:00:02 prepare start app=620
12:01:40 prepare result app=620 ready
12:05:00 prepare reserved app=400 title=FAKE78025
12:05:01 prepare start app=400
"""
DIGEST = """== session-2.log
PW_REPORT/1 profile=playden-steam-620 time=1000
WINE 0124:err:module:import_dll Library d3dx9_43.dll not found
WINE 0124:err:seh:fixme something
PW_WINE64 alive vk_shown=412 audible=9 frames_put=400
Crash!!! at 0xdead
== session-1.log
PW_REPORT/1 profile=playden-steam-620 time=500
all fine
== reports
session-2.log bytes=900 time=1042
session-1.log bytes=100 time=560
"""


class Parsing(unittest.TestCase):
    def test_ps_rows_and_running_titles(self):
        rows = playden.parse_ps(PS)
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0]["title"], "")
        self.assertEqual(rows[3], {"pid": 151, "ppid": 88, "app_id": "0x60000032", "title": "FAKE78024",
                                   "command": "eboot.bin"})
        self.assertEqual(playden.running_titles(rows, SANDBOX), {"NPXS40087", "FAKE77031", "FAKE78024"})

    def test_ls_keeps_names_with_spaces(self):
        rows = playden.parse_ls("d            0 kits\n-         1234 title-FAKE78024.log\n-            7 a b.txt\n")
        self.assertEqual(rows, [("d", 0, "kits"), ("-", 1234, "title-FAKE78024.log"), ("-", 7, "a b.txt")])

    def test_prepare_state_and_title(self):
        self.assertEqual(playden.reserved_title(RUNTIME, 620), "FAKE78024")
        self.assertIsNone(playden.reserved_title(RUNTIME, 62))
        self.assertEqual(playden.prepare_state(RUNTIME, 620)[0], "ready")
        self.assertEqual(playden.prepare_state(RUNTIME, 400)[0], "preparing")
        self.assertEqual(playden.prepare_state(RUNTIME, 62), (None, ""))
        self.assertEqual(playden.prepared_games(RUNTIME),
                         {"620": ("FAKE78024", "ready"), "400": ("FAKE78025", "preparing")})

    def test_sessions_newest_first_with_fatal_lines(self):
        newest, older = playden.sessions(DIGEST)
        self.assertEqual((newest["name"], newest["seconds"]), ("session-2.log", 42))
        self.assertEqual([p.split(":")[0] for p in newest["problems"]], ["missing dll", "crash"])
        self.assertEqual(newest["wine_errors"], {"module": 1, "seh": 1})
        self.assertEqual(playden.alive_stats(newest["text"])["vk_shown"], 412)
        self.assertEqual((older["seconds"], older["problems"]), (60, []))
        self.assertNotIn("bytes=900", newest["text"] + older["text"])

    def test_launch_outcome(self):
        self.assertIsNone(playden.launch_outcome("title=FAKE78024 wait state=0"))
        self.assertTrue(playden.launch_outcome("wait state=0 sceLncUtilLaunchApp rc=0x00000001"))
        self.assertFalse(playden.launch_outcome("sceLncUtilLaunchApp rc=0x80940010 sceSystemServiceLaunchApp rc=0x80940010"))
        self.assertFalse(playden.launch_outcome("bad title id"))


class Privacy(unittest.TestCase):
    def test_redacts_account_and_steam_ids(self):
        text = 'L"SteamUser" L"Greg42"\npath C:/users/greg42/x id 76561198000000001'
        out = playden.redact(text, playden.account_names([text]))
        self.assertNotIn("reg42", out.lower())
        self.assertNotIn("7656119", out)

    def test_sign_in_folder_is_refused(self):
        for path in ("/data/playden/user", "/data/playden/user/session.v1", "/data//playden/user/x",
                     "/tmp/session.v1"):
            with self.assertRaises(playden.ConsoleError):
                playden.allowed(path)
        self.assertEqual(playden.allowed("/data/playden/logs/runtime.log"), "/data/playden/logs/runtime.log")
        self.assertEqual(playden.allowed("/data/playden/username"), "/data/playden/username")


class Screen(unittest.TestCase):
    def test_black_still_moving(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as folder:
            def picture(name, colour, box=None):
                image = Image.new("RGB", (640, 360), colour)
                if box:
                    image.paste((255, 255, 255), box)
                image.save(Path(folder) / name)
                return Path(folder) / name
            black, grey = picture("a.png", (2, 2, 2)), picture("b.png", (90, 90, 90))
            moved = picture("c.png", (90, 90, 90), (0, 0, 320, 360))
            self.assertEqual(playden.measure(black, black)["verdict"], "black")
            self.assertEqual(playden.measure(grey, grey)["verdict"], "still")
            self.assertEqual(playden.measure(grey, moved)["verdict"], "moving")
            self.assertNotIn("verdict", playden.measure(grey))


class Flow(unittest.TestCase):
    """begin, watch, shot, verdict and summary against a fake console."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        reports = Path(self.tmp.name) / "reports"
        self.now = [0.0]
        self.quit_at = None
        patches = [
            mock.patch.object(playden, "REPORTS", reports),
            mock.patch.object(playden, "CURRENT", reports / "current.txt"),
            mock.patch.object(playden, "runtime_log", lambda: RUNTIME),
            mock.patch.object(playden, "is_running", lambda title: self.quit_at is None or self.now[0] < self.quit_at),
            mock.patch.object(playden, "screenshot", self.fake_shot),
            mock.patch.object(playden.time, "monotonic", lambda: self.now[0]),
            mock.patch.object(playden.time, "sleep", lambda s: self.now.__setitem__(0, self.now[0] + s)),
            mock.patch("builtins.print"),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        self.folder = reports / "620-portal-2"

    def fake_shot(self, target):
        from PIL import Image
        Image.new("RGB", (64, 36), (int(self.now[0]) % 200 + 40,) * 3).save(target)
        return Path(target)

    def run_json(self):
        return json.loads((self.folder / "run.json").read_text(encoding="utf-8"))

    def test_a_game_that_works(self):
        self.assertEqual(playden.main(["begin", "620", "Portal 2"]), 0)
        self.assertEqual(self.run_json()["title"], "FAKE78024")
        self.assertEqual(playden.main(["watch"]), 0)
        watch = self.run_json()["watch"]
        self.assertEqual(watch["status"], "survived")
        self.assertEqual(watch["screenshots"], ["screenshot-30s.png", "screenshot-90s.png", "screenshot-90s-b.png"])
        self.assertEqual(playden.main(["shot", "Main menu", "--note", "Options: the main menu"]), 0)
        self.assertEqual(playden.main(["note", "Cross on New Game: the intro plays"]), 0)
        self.assertTrue((self.folder / "step-01-main-menu.png").is_file())
        self.assertEqual(playden.main(["verdict", "works", "--played", "gameplay", "--model", "Test Model",
                                       "--summary", "Reaches gameplay."]), 0)
        issue = (self.folder / "issue.md").read_text(encoding="utf-8")
        self.assertIn("# [Test] Portal 2", issue)
        self.assertIn("https://store.steampowered.com/app/620", issue)
        self.assertIn("✅ Works", issue)
        self.assertIn("This report is AI generated using Test Model.", issue)
        self.assertIn("2. Options: the main menu (`step-01-main-menu.png`)", issue)
        self.assertIn("- [ ] I own this game", issue)
        self.assertEqual(json.loads((self.folder / "result.json").read_text(encoding="utf-8"))["played"], "gameplay")
        self.assertEqual(playden.main(["summary"]), 0)
        self.assertIn("| Portal 2 | 620 | works | gameplay |",
                      (playden.REPORTS / "summary.md").read_text(encoding="utf-8"))

    def test_a_game_that_quits_early(self):
        playden.main(["begin", "620", "Portal 2"])
        self.quit_at = 12.0
        self.assertEqual(playden.main(["watch"]), 1)
        watch = self.run_json()["watch"]
        self.assertEqual(watch["status"], "exited_early")
        self.assertEqual(watch["screenshots"], ["screenshot-after-exit.png"])

    def test_a_game_that_never_appears(self):
        playden.main(["begin", "620", "Portal 2"])
        self.quit_at = 0.0
        self.assertEqual(playden.main(["watch"]), 1)
        self.assertEqual(self.run_json()["watch"]["status"], "did_not_start")

    def test_untested_writes_no_draft(self):
        playden.main(["begin", "620", "Portal 2"])
        self.assertEqual(playden.main(["verdict", "untested", "--model", "M", "--summary", "Could not launch."]), 0)
        self.assertFalse((self.folder / "issue.md").exists())
        self.assertTrue((self.folder / "result.json").is_file())


if __name__ == "__main__":
    unittest.main()
