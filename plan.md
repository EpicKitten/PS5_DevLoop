# DevLoop capability roadmap

Growing DevLoop from a virtual pad and screen capture into a full remote
testing and control API for a jailbroken PS5, over the one text-per-connection
protocol on port 9112. Each capability below is grounded in a callable SDK
function (a stub library in the PS5 payload SDK, or a `ps5/*.h` kernel helper).

Legend: **done** = implemented and verified on firmware 12.20; **new** = added
in this pass; **staged** = needs a resource or more work (noted per item).

## Done (earlier)

- **Input** — virtual DualSense: `open`/`close`, `press`, `hold`, `release`, `seq`.
- **Sight** — `screenshot` (whole screen), `frame` (a title's own frame),
  `list-captures`/`latest-capture`/`delete-capture`, `kernel-log`.
- **Files** — `ls`, `get`, `put`, `rm`, `mv` (sandbox-escaped).
- **Processes** — `ps` (pid, app id, title id, command).
- **Apps** — `launch` (registered titles), `close-app`, `uninstall`.
- **Browser** — `open-url`.
- **Power** — `rest`, `reboot`, `power-off`.
- **Setup** — `doctor`, `ping`, `raw`, `load`.

## New (this pass)

| Command | Function(s) | What it gives |
| --- | --- | --- |
| `notify "MSG"` | `sceKernelSendNotificationRequest` | a toast on the TV: a feedback channel for an unattended loop |
| `hwinfo` | `sceKernelGetCpuTemperature`, `sceKernelGetSocSensorTemperature`, `sceKernelGetCpuFrequency`, `sceKernelGetHwModelName`, `sceKernelGetHwSerialNumber` | model, temps and clock: health during long runs |
| `df` | `getmntinfo` / `statfs` | free space per mounted volume |
| `kill PID` | `kill(pid, SIGKILL)` | end any process, not just registered apps |
| `peek PID ADDR LEN` | `mdbg_copyout` (ps5/mdbg.h) | read a running process's memory |
| `poke PID ADDR HEX` | `mdbg_copyin` | write a running process's memory |
| `http URL` | raw socket HTTP/1.1 GET | let the payload fetch a URL (http://) |

`peek`/`poke` are the standout for game testing: read a title's state (score,
health, level) and assert on it from the host, or set it. `ping` reports the
address of a static probe buffer, so the memory path is verifiable: `peek` reads
it back correctly. `poke` to the payload's *own* process is refused (`EPERM`) by
the kernel's self-write rule — it is meant for another process (a game), which
is the real use. Verify it against a running title with a known address.

## Staged (need a resource or more work)

- **`type "TEXT"`** — text entry. `sceImeDialog*` (libSceImeDialog) opens the
  on-screen keyboard and returns a string, but injecting into an arbitrary
  focused field from a payload is unreliable; `sceKeyboard*` HID is the other
  route. Needs a focused app to test against. *Deferred: reliability.*
- **`setting get/set KEY [VALUE]`** — system settings via `sceRegMgrGetInt/SetInt/
  GetStr/SetStr` (libSceRegMgr): resolution, HDR, language, rest timers. Reads
  are safe; writes need an allowlist of known-safe keys so a bad key can't
  misconfigure the console. *Deferred: needs the key allowlist.*
- **Live video / framebuffer** — continuous capture, not stills, to drop the
  capture-card dependency. libSceVideoOut / libSceAgc expose the display
  pipeline, but reading the *composited* output from a payload is the known hard
  problem (no avcapture stub; `SceAvCapture` is a separate service). *Deferred:
  research.*
- **Call into a running process** — `kernel_dynlib_dlsym` / `_resolve` resolve
  symbols in another process; with `poke` that can invoke or hook functions.
  Powerful, fiddly, risky. *Deferred: advanced.*
- **Install a package** — `sceAppInstUtilAppInstallPkg` plus the installer's
  register-and-poll flow. *Deferred: needs a `.pkg` to build against.*
- **Wake from standby** — a host-side Remote Play magic packet (libSceRemoteplay
  on the console side is only part of it). *Deferred: needs pairing credentials.*
- **Pad LED / rumble** — `scePad` lightbar and rumble as a physical test signal.
  Low value for the *virtual* pad (no lightbar), so parked. *Deferred: low value.*

## Notes

- No trophy or save-data stub libraries ship in this SDK, so that automation is
  not a quick win here.
- Memory and process commands run with the payload's elevated credentials.
  `kill`, `poke` and (future) `setting set` can break a running title; use them
  deliberately.
