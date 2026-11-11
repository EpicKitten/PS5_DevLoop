# PS5 DevLoop

[![stars](https://img.shields.io/github/stars/griches/PS5_DevLoop?style=flat)](https://github.com/griches/PS5_DevLoop/stargazers)
[![licence](https://img.shields.io/badge/licence-GPL--3.0-blue)](LICENSE)

**Let an AI agent, or a script, drive a jailbroken PS5 by itself: press buttons,
see the screen, repeat.**

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
- **Sight**: the running title's own frame in about a second, or a screenshot of
  the whole screen, system UI included, in about three. Both arrive as a small
  PNG an agent can read, and neither leaves anything on the console.

With those two, an agent can launch a port, walk its menus, notice what is
wrong, change the code, and try again, with nobody at the pad.

```bash
tools/ps5loop.py load                  # once per console boot
tools/ps5loop.py open                  # connect the virtual controller
tools/ps5loop.py seq l1 wait:1000 cross wait:14000    # Games tab, launch
tools/ps5loop.py frame                 # the title's frame -> captures/capture-<time>.png
tools/ps5loop.py seq down down cross:300 wait:1000
tools/ps5loop.py screenshot            # the whole screen, for the home screen and dialogs
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

`AGENTS.md` is the operating guide: the loop, which picture to take when, the
rules that keep the console safe, and how to give a new title the `frame` hook
(a few lines). `CLAUDE.md` includes it, so Claude Code reads it on entering the
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
| `frame [FILE]` | the running title's frame (`--title`, from `titles.json`) |
| `screenshot [FILE]` | the whole screen, through the console's screenshot |
| `kernel-log [MS] [FILE]` | dump the console's kernel log (a bring-up aid) |
| `delete-capture PATH` | remove a leftover capture through the gallery |
| `doctor` | check the host, payload, loader and capture tools |
| `close` | remove the pad |
| `pads` / `--pad N` | up to four virtual pads: `--pad 2` (to 4) on `open`, `close`, `status`, `press`, `hold`, `release` and `seq` drives another one, `pads` lists them, `close --all` removes them all. Pads 2-4 go to the next signed-in user without a pad and never disconnect a real controller; on the wire it is `pad N ACTION`, and plain `pad ACTION` stays pad 1 |

`tools/ps5loop.py --help` lists the rest; `payload/main.c` documents the wire
protocol (one text command per connection, port 9112).

## Controlling the console

Beyond input and sight, the payload drives the console through its own services,
run with the payload's credentials. Paths are the console's real paths; a shell
that rewrites absolute paths (Git Bash on Windows) mangles them, so quote or
disable that first.

| Command | What it does |
| --- | --- |
| `ps` | list every process, with the app id and title id of the apps |
| `ls PATH` | list a console directory |
| `get REMOTE [LOCAL]` | download a console file (default `downloads/<name>`) |
| `hash PATH` | SHA-256 of a console file, computed on the console (hex) |
| `put LOCAL REMOTE` | upload a local file to the console |
| `rm PATH` / `mv SRC DST` | delete, move or rename a console file |
| `open-url URL` | open a URL in the console's web browser |
| `launch TITLE_ID` | launch a registered title |
| `close-app [APP_ID]` | close a running app (id from `ps`, else the foreground one) |
| `install /PATH.pkg` | install a `.pkg` already on the console |
| `exec /PATH [ARGS]` | run a console binary and send back its output and exit code |
| `wake` | wake the console from standby (UDP packet; needs `PS5_WAKE_CREDENTIAL`) |
| `locate-title` | the foreground app's ids right now (app id, pid, title id, command) |
| `module PID NAME` / `modules PID` / `dlsym PID NAME SYM` | a named module's mapping, every module in a process, or a symbol's address |
| `rest` / `reboot` / `power-off` | rest mode, reboot, or power off |
| `uninstall TITLE_ID` | uninstall a title |
| `notify TYPE MESSAGE` | an on-screen message: `toast` (banner) or `flyout` (card) |
| `trophy GRADE NAME[\|SUB]` | a notification in the system's trophy style: the overlay's own `InteractiveToastTrophy` template, the grade's artwork from `/system_ex/vsh_asset` (bronze, silver, gold, platinum) and its trophy sound. Only a notification: nothing is unlocked, recorded or synced |
| `hwinfo` | model, serial, CPU temp, all 64 SoC sensors (incl. the hottest), CPU clock (and each core's, `sceKernelGetCpuCoreClock`), fan speed %, SoC power (mW, total and per rail), system version string, SoC id, CPU mode, core count, uptime |
| `hwdetail` | JSON (also `GET /api/hwdetail`): the ICC's boot and shutdown counts, operating time, power-up cause, firmware ("Floyd") version, error log (records stamped in seconds since 2012-01-01) and system event log, the last boot's stage timestamps, the secure clock (`/dev/srtc`), USB and disc-drive power states, the SMU's clock table, the PCIe AER and GDDR6 error-rate counters, and system facts (ICC model, AMD part number, build number, budgets). Every read is a libkernel "get"; what is not decoded is sent as hex |
| `display` | JSON (also `GET /api/display`): the HDMI sink's raw EDID (`sceVideoOutGetHdmiRawEdid_`, port 0x7000: byte 0 is the block count) and the system's 0x144-byte monitor-info record (`sceVideoOutGetHdmiMonitorInfo_`). The dashboard decodes the EDID: name, maker, preferred mode, video modes, HDMI 2.x (TMDS/FRL, VRR, ALLM, DSC), HDR and colorimetry, audio |
| `thermal-probe` | bring-up: the raw readings libkernel's other thermal paths give (the ICC's `/dev/icc_thermal` byte, the `machdep.aeolia.alert.thermal` sysctl, the SMU's read-only getters), uninterpreted |
| `df` | free space per mounted volume |
| `led BWA` / `off` / `auto` / `breathe B W A [MS]` / `chase [MS]` | the console indicator LED (static, off, system-controlled, or an animated effect), via `/dev/icc_indicator` |
| `fan [TEMP]` | read the fan duty, or set the manual threshold (30..90 °C), via `/dev/icc_fan` |
| `fan hold TEMP [SECS]` / `off` | re-apply the threshold every `SECS` s (default 7) in a background thread — a one-shot `fan set` is reverted by the system, so hold keeps it pinned (idea from [owendswang/ps5-fan-control](https://github.com/owendswang/ps5-fan-control)) |
| `beeper [PATTERN]` / `mute 0\|1` / `dim 0..2` / `status` | beep the console, set its mute or dim level, or read both back (`sceKernelIccSetBuzzer*`; the ICC's `0x4001950d`/`0x4001950f` getters). Dim is a byte the ICC interprets: by ear on a CFI-2015, 0 is the loudest and sounds like a different speaker, 1 and 2 are softer in the same tone; `vol` is an alias |
| `kill PID` | end a process (not just a registered app) |
| `peek PID ADDR LEN` / `poke PID ADDR HEX` | read / write a process's memory (writes are read back and verified) |
| `scan PID ADDR LEN W VALUE` | find a `W`-byte integer (`W` = 1/2/4/8) in a span of a process's memory |
| `scan-aob PID ADDR LEN PATTERN` | find a byte pattern (`??` = wildcard) in a span |
| `kern-read ADDR LEN` / `kern-write ADDR HEX` | read / write kernel memory by absolute address (writes verified) |
| `klog-stream [MS]` | stream the kernel log live until you disconnect |
| `discover [--timeout S]` | find consoles on the LAN by UDP broadcast to the command port |
| `http URL` | fetch an `http://` URL (saved under `downloads/`) |
| `settings` | read the system registry (settings) as JSON |
| `setting-set REGID VALUE` | write one integer setting by its regId (read back and verified) |

Verified on firmware 12.20: `ps`, `ls`/`get`/`put`/`rm`/`mv` (a round trip is
byte-for-byte), `hash` (matches `sha256sum` for a 1 MB file and an empty file),
`open-url`, `close-app`, `locate-title`, `module`/`modules`/`dlsym`, `call`
(ptrace thread-hijack, proved against a disposable sleeper: `getpid`→its own pid,
`abs(-5)`→5, target survived), `hwinfo`
(all 64 SoC sensors answer), `df`, `led`/`fan`/`beeper` (ICC ioctls/calls return
success, including the `breathe`/`chase`/`auto` effects and beeper `mute`/`dim`),
`scan`/`scan-aob` (hit a known probe), `kern-read`/`kern-write` and `setting-set`
(verified), `notify`, `peek`, `http`, the UDP beacon and `klog-stream`, and the
console-hosted dashboard with its control panel (`GET /`, `GET /api/hwinfo`,
`GET /api/hwdetail` and `GET /api/display` — the Hardware and Display sections — and
`GET /api/run?c=CMD` — verified on 12.20 with `ping`, `hwinfo` and `ls`, and the
`put`/`quit`/`klog-stream` exclusion refused with 400). `install` and `exec` are
wired and fail gracefully on a missing target (a positive run needs a real
`.pkg` / binary); `wake` and rest-mode survival are host/standby paths not
exercised in this session.
Notes and limits:

- **`peek`** reads a process's memory; **`poke`** writes it. Writing the
  payload's own process is refused by the kernel's self-write rule — `poke`
  targets another process (a game), which is the point; verify it against a
  running title. `ps` gives the pids. `plan.md` tracks what is next.
- **`notify toast`** draws the small banner, **`notify flyout`** the larger
  card. Both are drawn by the system, so they work from the payload. Modal
  dialogs (message, error, on-screen keyboard) are not here: they render into
  the calling app's own framebuffer, which a background payload does not have.
- **`lightbar R G B`** attempts the *physical* DualSense lightbar as a
  diagnostic. On 12.20 scePad initialises (`init=0`) but `scePadOpen` returns
  `0x809b0081` (ALREADY_OPENED): SceShellUI owns the pad, so the payload cannot
  set its lightbar/haptics. The console's own body LED (`led`, via
  `/dev/icc_indicator`) is separate and does work.
- **`settings`** reads the registry by scanning regIds (`(cat << 24) | (sub <<
  16) | entry`) with `sceRegMgrGetInt`/`GetStr`, keyed by regId in hex. It
  covers the readable int and string values across every category; binary
  entries and writing values are not included (a write needs a safe-key
  allowlist — see `plan.md`).

- **`launch`** uses `sceSystemServiceLaunchApp`, which takes titles registered in
  the shell's app database. Homebrew launched by a loader or a launcher (the
  `FAKE*` titles under `/data/homebrew`) are not registered and answer
  `0x80940005`; a real installed title (a `PPSA*` id) launches.
- **`rest` / `reboot` / `power-off` / `uninstall` are not reversible from here.**
  They are implemented but were not triggered in testing (`off`/`reboot` end the
  payload; `uninstall` removes a title). Power replies before it acts, since the
  request may end the process first.
- **Install a package** and **wake from standby** are not here yet: install needs
  the app installer's full register-and-poll flow and a `.pkg` to build against;
  wake is a host-side Remote Play packet that needs the console's pairing
  credentials.

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
and **every SoC sensor** (gauges, a grid of all readings, and a rolling chart of
the CPU and **all 64 SoC sensors**), the CPU clock, and a **fan-speed gauge and
rolling chart** (from `sceKernelGetCurrentFanDuty`, not the manual-duty ioctl,
which reads 0 in auto mode). The charts share one time range, from 5 s to 1 h;
on the 5 s and 30 s ranges the page samples every 0.5 s instead of every 2 s.

Below it is a control panel that drives **every function the payload has**
— the virtual pad (open/close and each DualSense button), frame and screenshot
(shown inline), `ps`/`ls`/`get`/`hash`/`rm`/`mv`, launch/close/uninstall/install,
`open-url`, `notify`, the LED/fan/beeper, the memory and process tools
(`peek`/`poke`/`scan`/`call`/`kern-*`/`modules`/`dlsym`), settings, `exec`, and
power. Destructive actions (power, uninstall, install, `rm`, `poke`,
`kern-write`, `kill`, `exec`, `setting-set`) ask for confirmation first. A raw
command box runs anything verbatim.

Each control issues `GET /api/run?c=<command>`; the payload runs that command
(the same dispatch the text protocol uses) and returns its reply — a picture for
`frame`/`screenshot`, text otherwise. `put` (upload), `quit` and `klog-stream`
are not offered over HTTP (they need a connection a browser cannot give); use the
CLI for those. The payload has no authentication, so serve it only on a network
you trust — the web panel exposes nothing the command port did not already.

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
- **A title's frame** (`frame`) comes from the title. The payload creates a
  request file in the title's folder; the title removes it and writes a picture
  of its next frame; the payload sends that and deletes it. About 1 s end to end
  at 3840x2160. A title needs a hook of a few lines (`AGENTS.md`, "Giving a
  title a frame"), and an entry in `titles.json`.
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
- `sceScreenShotCapture` (libSceScreenShot), called from the payload, answers
  "busy" until it is cancelled, with either type: no way round the Create hold
  was found.
- **Firmware 12.20**: the pad path above is identical, confirmed live (`add`
  `0x803b0006`, device id = `InsertData` handle, `bind` 0, input driving a
  homebrew launcher once the real pad is evicted). This console runs no klog
  server, so the payload reads `/dev/klog`; `screenshot`'s Create hold and its
  `sceContentDeleteByPath` cleanup both work. The direct screenshot service
  (`direct-screenshot`) answers `0x80be000a`, so as on 13.60 only the Create
  hold works. Use `ps5loop.py kernel-log` to read the log where the server is absent.

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
