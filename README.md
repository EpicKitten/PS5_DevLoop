# PS5 DevLoop

[![stars](https://img.shields.io/github/stars/griches/PS5_DevLoop?style=flat)](https://github.com/griches/PS5_DevLoop/stargazers)
[![licence](https://img.shields.io/badge/licence-GPL--3.0-blue)](LICENSE)

**Let an AI agent, or a script, drive a jailbroken PS5 by itself: press buttons,
see the screen, repeat.**

> [!IMPORTANT]
> **This copy is set up for Playden compatibility testing.** `tools/playden.py`
> adds the test around DevLoop's pad and screen: the 90-second watch, Playden's
> logs (redacted), and a report folder per game with a draft for Playden's
> test-report form. `AGENTS.md` is the tester's guide; `DEVLOOP.md` is DevLoop's
> own. Below is DevLoop's README, cut to what this copy keeps, where
> "`AGENTS.md`" means `DEVLOOP.md` here.
>
> **What was dropped.** The payload, its client (`tools/ps5loop.py`) and the
> dashboard page here have only the commands that testing Playden games uses.
> Gone from all three: `frame` (a Playden game cannot be given the hook),
> `launch`, `install` and `uninstall` (PS5 packages; Playden's games go through
> Playden), `rest`/`reboot`/`power-off`, `open-url`, `notify`, `trophy`, `led`,
> `fan`, `beeper`, `lightbar`, `keyboard`, `hwdetail`, `display`,
> `settings`/`setting-set`, `http`, `hash`, `exec`, and the memory tools (`peek`,
> `poke`, `scan`, `scan-aob`, `kern-read`, `kern-write`, `module`, `modules`,
> `dlsym`, `call`). The payload answers `err unknown` to them, so nothing here
> can power the console off or write to memory. `build/ps5loop-devloop-full.elf`
> is DevLoop's own payload, kept in case one of them is wanted back.

> [!NOTE]
> **This fork's changes are 100% vibecoded.** DevLoop itself is
> [Gary Riches' project](https://github.com/griches/PS5_DevLoop); everything this
> fork adds to it, code and documentation, was written by an AI agent and checked
> against a real console rather than reviewed line by line. Read it with that in
> mind before you run it on yours.

Porting homebrew to the PS5 has had a gap in the middle of every loop. A machine
can build a title, deploy it, launch it and read its log, but it cannot press
Cross on the menu, and it cannot see that the screen is black. So a person sits
with a controller, and every long task stops to wait for them.

DevLoop closes that gap. It is one small payload that stays resident on the
console, and one Python client:

- **Input**: a system-wide virtual DualSense. The home screen and every title
  see a real controller.
- **Sight**: a screenshot of the whole screen, system UI included, in about
  three seconds. It arrives as a small PNG an agent can read, and leaves nothing
  on the console.

With those two, an agent can launch a port, walk its menus, notice what is
wrong, change the code, and try again, with nobody at the pad.

```bash
tools/ps5loop.py load                  # once per console boot
tools/ps5loop.py open                  # connect the virtual controller
tools/ps5loop.py seq l1 wait:1000 cross wait:14000    # Games tab, launch
tools/ps5loop.py screenshot            # the whole screen -> captures/capture-<time>.png
tools/ps5loop.py seq down down cross:300 wait:1000
tools/ps5loop.py screenshot
tools/ps5loop.py close
```

That sequence, on a real console: switch to the Games tab, launch a Quake III
port, go through its menus, quit through "Exit game?", and land back on the
home screen, every step checked from a picture.

> For homebrew development on a console you own. It is tested on **firmware
> 12.20 and 13.60**; the calls it relies on are undocumented and may differ
> elsewhere. `payload/main.c` reports what every step returned, and `ps5loop.py
> kernel-log` dumps the kernel log, so that another firmware can be brought up.

## For agents

`AGENTS.md` is the tester's guide: one game from start to finish, how to reach
a result, and the rules that keep the console safe. `CLAUDE.md` includes it, so Claude Code reads it on entering the
folder. Point any other agent at it first.

## What you need

- A jailbroken PS5 on your network, with an **ELF loader** (port 9021) running,
  as the usual payload chains provide. A **klog server** (port 3232) is used if
  one is running; without it the payload reads `/dev/klog` directly, so neither
  the pad's device id nor controller eviction depends on it (firmware 12.20
  runs none).
- Python 3 on the host, and for pictures `JxrDecApp` (jxrlib) and `magick`
  (ImageMagick): `brew install jxrlib imagemagick`, or your distribution's
  packages.
- To build: the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk).
  `tools/build.sh` finds it through `PS5_PAYLOAD_SDK`; `BUILDING.md` walks
  through building and deploying, Windows included.
- The console's address in `PS5_HOST`, or in `.env` (copy `.env.example`).

DevLoop presses buttons and looks. Launching and closing titles from a script,
and reading klog, are jobs for the tools you already have; mine are
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)'s `ps5vkctl` and run
tools. The pad can launch anything from the home screen without them.

## Commands

| Command | What it does |
| --- | --- |
| `load [ELF]` | send the payload to the console's ELF loader |
| `open` | create the virtual pad on the foreground user and make it the one titles read |
| `press BUTTONS [--ms N]` | press and release: `cross`, `l1+r1` |
| `hold BUTTONS [--lx N ...]` | hold buttons, sticks (0-255) and triggers until the next `hold` or `release` |
| `seq STEPS...` | presses and waits in order: `down cross:300 wait:1000` |
| `screenshot [FILE]` | the whole screen, through the console's screenshot |
| `kernel-log [MS] [FILE]` | dump the console's kernel log (a bring-up aid) |
| `delete-capture PATH` | remove a leftover capture through the gallery |
| `doctor` | check the host, payload, loader and capture tools |
| `close` | remove the pad |
| `pads` / `--pad N` | up to four virtual pads: `--pad 2` (to 4) on `open`, `close`, `status`, `press`, `hold`, `release` and `seq` drives another one, `pads` lists them, `close --all` removes them all. Pads 2-4 go to the next signed-in user without a pad and never disconnect a real controller; on the wire it is `pad N ACTION`, and plain `pad ACTION` stays pad 1 |

`tools/ps5loop.py --help` lists the rest; `payload/main.c` documents the wire
protocol (one text command per connection, port 9112).

## Controlling the console

Beyond input and sight, the payload reads and changes the console through its
own services. Paths are the console's real paths; a shell that rewrites absolute
paths (Git Bash on Windows) mangles them, so quote or disable that first.

| Command | What it does |
| --- | --- |
| `ps` | list every process, with the app id and title of the apps (on firmware 12.20 the title column shows only its number: `77031` for `FAKE77031`) |
| `locate-title` | the foreground app's ids right now (app id, pid, title, command) |
| `close-app [APP_ID]` | close a running app (id from `ps`, else the foreground one) |
| `kill PID` | end a process (not just a registered app) |
| `ls PATH` | list a console directory |
| `get REMOTE [LOCAL]` | download a console file (default `downloads/<name>`) |
| `put LOCAL REMOTE` | upload a local file to the console |
| `rm PATH` / `mv SRC DST` | delete, move or rename a console file |
| `df` | free space per mounted volume |
| `hwinfo` | model, firmware, CPU and SoC temperatures, CPU clock, fan speed, SoC power: what the console was doing while a game ran |
| `klog-stream [MS]` | stream the kernel log live until you disconnect |
| `discover [--timeout S]` | find consoles on the LAN by UDP broadcast to the command port |
| `wake` | wake the console from standby (UDP packet; needs `PS5_WAKE_CREDENTIAL`) |
| `raw WORDS...` | send any command line to the payload as it is |

## Live dashboard

The payload hosts a live hardware dashboard **and a control panel** itself: it
answers HTTP on the same port as the command protocol (9112), so a browser opens
it directly.

```
http://<console>:9112/           # the dashboard
http://<console>:9112/api/hwinfo # the JSON it polls
http://<console>:9112/api/run?c=CMD  # run any command, get its reply
```

The top of the page is the live hardware view: the model, the CPU temperature
and every SoC sensor, the CPU clock, the fan speed and SoC power, with rolling
charts. It shows what the console is doing while a game runs.

Below it is a control panel for what the payload has: the virtual pad
(open/close, each DualSense button, the sticks and triggers, and a **Type** box
that steers a game's on-screen keyboard key by key), screenshot and the
captures gallery (shown inline), `ps`, `locate-title`, `df`, `hwinfo`,
`close-app`, `kill`, the kernel log, and `ls`/`get`/`mv`/`rm`. `rm`, `kill`
and deleting a capture ask first. A raw command box runs anything verbatim.

Each control issues `GET /api/run?c=<command>`; the payload runs that command
and returns its reply, a picture for `screenshot`, text otherwise. `put`
(upload), `quit` and `klog-stream` are not offered over HTTP; use the CLI for
those. The payload has no authentication, so serve it only on a network you
trust.

The page is embedded in the payload (`tools/ps5web.html` →
`payload/dashboard_html.h`, regenerated by `tools/embed_dashboard.py`), so no
host-side server is needed.

`tools/ps5web.py` is an optional proxy, for viewing on localhost or keeping the
console's address out of the browser:

```bash
tools/ps5web.py            # serves http://127.0.0.1:8099, forwards to the console
```

To try an edit to the page without rebuilding the payload, serve the local file
instead of the console's copy; the API calls still go to the console:

```bash
tools/ps5web.py --html tools/ps5web.html
```

It uses the same `PS5_HOST`/`.env` as `ps5loop.py`, and never sends the console's
address to the browser.

## How it works

- **Input** is the system's virtual pad device (`scePadVirtualDevice*`, what
  Remote Play uses). The payload holds a state and inserts it every 8 ms;
  presses are timed on the console, not across the network.
- **The whole screen** (`screenshot`) is the console's own screenshot: the payload
  holds the virtual Create button for 1.2 s, waits for the new file under
  `/user/av_contents/photo`, sends it, and deletes it through
  `sceContentDeleteByPath` (libSceContentDelete), as the gallery's own Delete
  does: the file, its thumbnail and its row in the media database all go. The
  client decodes it (JPEG XR when HDR is on) and scales it to 1280 wide.

## What the console showed

Measured live. Firmware 12.20 and 13.60 behave the same unless noted.

- `AddDevice` for the real user answers `0x803b0001` and creates nothing. For
  user 1 it answers `0x803b0006` and the device appears in klog
  (`DEVICE_ADDED ... subType:22`); its id is the handle `InsertData` takes.
- `sceMbusBindDeviceWithUserId`, called in the payload's own process, binds it
  (returns 0, `DEVICE_OWNER_CHANGED` follows). No injection into the shell is
  needed.
- The console still raises "Who's using this controller?". Circle from the
  virtual pad closes it; `open` does that 1.5 s after opening.
- A title opens its user's first controller (`scePadOpen(user, 0, 0)`). With a
  real DualSense connected the virtual pad is the second and the title never
  reads it. `sceMbusDisconnectDevice` on the real one leaves the virtual pad
  first, and a title already running picks it up at once. The payload reads the
  login manager's `[CIM]` controller table from the kernel log (a fresh reader
  is replayed the backlog) and disconnects the real pad itself, so `open` needs
  no klog server; PS on the real pad reconnects it.
- Confirmed by picture, on the home screen and in a title's menus: Cross,
  Circle, the d-pad, L1, Create, PS. Not yet exercised: sticks, triggers,
  Options, the touchpad.
- **Firmware 12.20**: the pad path above is identical, confirmed live (`add`
  `0x803b0006`, device id = `InsertData` handle, `bind` 0, input driving a
  homebrew launcher once the real pad is evicted). This console runs no klog
  server, so the payload reads `/dev/klog`; `screenshot`'s Create hold and its
  `sceContentDeleteByPath` cleanup both work. Use `ps5loop.py kernel-log` to read the log where the server is absent.

## Limits

- Pictures are stills, not video. Nothing known reads the composited display
  from a payload.
- `screenshot` takes about 3 s: Create held for less than 1.2 s opens the Create
  menu instead.
- While the pad drives a title, the real controller is disconnected
  (`open --beside` keeps it, for the home screen only).
- The pad's device id, and the controller table `open` evicts from, are read
  from the console's kernel log (its klog server if one runs, else `/dev/klog`).
  Another reader holding `/dev/klog`, or a client connecting to the server at
  that moment, can make `open` miss them.
- The payload listens on the local network with no authentication. Run it on a
  network you trust.

## Credits

The virtual pad's call sequence and its 120-byte sample follow what
[SplashDown](https://github.com/seregonwar/SplashDown),
[Control4Free](https://github.com/MoHadiShibli/Control4Free),
[OmniPad-PS5](https://github.com/diegobarbosaa/OmniPad-PS5) and
[Ghostpad](https://github.com/srbraboo/Ghostpad) established on hardware. The
payload's command server follows `ps5vkctl` from
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) by Mihawk. Built with the
[PS5 payload SDK](https://github.com/ps5-payload-dev/sdk).

The dashboard page embeds [uPlot](https://github.com/leeoniya/uPlot) (MIT) for its
charts and [jpegxr](https://github.com/brion/jpegxr) (BSD-3-Clause; Microsoft's
jxrlib as WebAssembly, wrapped by Brion Vibber) to show the console's JPEG XR
captures; its licence is in `third_party/jpegxr-LICENSE.md`.

Two MCP projects that give an agent eyes and hands on a jailbroken PS5 informed
DevLoop's design, and are worth a look for anyone building in this space:

- [ps5-mcp](https://github.com/Darkatek7/ps5-mcp) by Darkatek7 — a Python MCP
  server with a virtual pad and direct Avcap2 capture, plus title, power, FTP
  and klog control, built to test homebrew without a capture card.
- [ps5-mcp](https://github.com/leolobato/ps5-mcp) by leolobato — a capture-card
  approach (as here) with `padd` over TCP, a shared-console claim queue, visual
  template matching, and input record/replay.

Both are GPL-3.0; no code from either is used here. Not affiliated with Sony
Interactive Entertainment.

## License

GPL-3.0-or-later. See `LICENSE`.
