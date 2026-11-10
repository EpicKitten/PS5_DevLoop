ideas for next boot (surveyed)

- `hash PATH` — SHA-256 of a console file, computed on the console. **Done** and
  verified on 12.20 (matches `sha256sum` for a 1 MB file and the empty-file
  vector). Lets a transfer be checked in place without pulling the file back.
- [PS5-Suite](https://github.com/manos555555/PS5-Suite) (GPL-3, Manos) surveyed.
  Borrowable ideas:
  - **Console LED** via `/dev/icc_indicator` — three channels (blue `0x01`,
    white `0x11`, amber `0x21`), breathe/chase effects, "auto" returns control to
    the system. A *direct device path*, not `scePad`, so it sidesteps the
    shell-owned-pad blocker that stops our lightbar work — worth trying from the
    payload.
  - **DualSense control** confirms our plan: it goes "through a remote bridge
    into SceShellUI" (the payload has no pad session), and lightbar colour is set
    that way — the same SceShellUI-bridge path our controller-output item needs.
  - **Fan control** (temperature threshold / presets), **beeper** (console beep +
    mute), **remote shell** (run a command, save the log), and **module listing**
    per process. Fan control and beeper are small, self-contained wins.


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
| `notify TYPE "MSG"` | `sceKernelSendNotificationRequest` (toast), `sceNotificationSend` (flyout) | an on-screen message: a `toast` banner or a `flyout` card — a feedback channel for an unattended loop |
| `hwinfo` | `sceKernelGetCpuTemperature`, `sceKernelGetSocSensorTemperature` (scans all 64 SoC sensors — all answer on 12.20 — reports each plus the hottest), `sceKernelGetCpuFrequency`, `sceKernelGetHwModelName`, `sceKernelGetHwSerialNumber` | model, full thermal picture and clock: health during long runs |
| `GET /`, `GET /api/hwinfo` | HTTP served on the command port (9112) from an embedded page | the live dashboard, **hosted by the console** (gauges, a 64-sensor grid, a rolling chart); `tools/ps5web.py` is now just an optional localhost proxy |
| `df` | `getmntinfo` / `statfs` | free space per mounted volume |
| `kill PID` | `kill(pid, SIGKILL)` | end any process, not just registered apps |
| `peek PID ADDR LEN` | `mdbg_copyout` (ps5/mdbg.h) | read a running process's memory |
| `poke PID ADDR HEX` | `mdbg_copyin` | write a running process's memory |
| `http URL` | raw socket HTTP/1.1 GET | let the payload fetch a URL (http://) |
| `settings` | `sceRegMgrGetInt`/`GetStr` | read the system registry (settings) as JSON |
| `hash PATH` | self-contained SHA-256 over the sandbox-escaped read path | SHA-256 of a console file, on the console — verify a transfer in place |

`peek`/`poke` are the standout for game testing: read a title's state (score,
health, level) and assert on it from the host, or set it. `ping` reports the
address of a static probe buffer, so the memory path is verifiable: `peek` reads
it back correctly. `poke` to the payload's *own* process is refused (`EPERM`) by
the kernel's self-write rule — it is meant for another process (a game), which
is the real use. Verify it against a running title with a known address.

## Staged (need a resource or more work)

- **Modal dialogs** — `sceMsgDialog` (OK / YesNo / progress), `sceErrorDialog`,
  and `sceImeDialog` (on-screen keyboard, for `type "TEXT"`) all exist on the
  console, but they are CommonDialog dialogs: they draw into *the calling
  process's* framebuffer and must be pumped from its render loop. A background
  payload has no video context, so these cannot be shown from here — only the
  system-drawn notifications (`toast`, `flyout`) can. *Deferred: a dialog needs
  to run inside a title's render loop (or an injected context).*
- **Write a setting** — reading is done (`settings` dumps the registry as JSON);
  writing with `sceRegMgrSetInt`/`SetStr` is held back until there is an
  allowlist of known-safe regIds, so a bad key can't misconfigure the console.
  The full enumeration (`sceRegMgrToolGetEntryCnt`) is refused to the payload
  (`0x800d0218`), so `settings` scans regIds instead; a true dump-all would need
  that tool privilege. *Deferred: the safe-key allowlist.*
- **Live video / framebuffer** — continuous capture, not stills, to drop the
  capture-card dependency. libSceVideoOut / libSceAgc expose the display
  pipeline, but reading the *composited* output from a payload is the known hard
  problem (no avcapture stub; `SceAvCapture` is a separate service). *Deferred:
  research.*
- **Call into a running process** — `kernel_dynlib_dlsym` / `_resolve` resolve
  symbols in another process; with `poke` that can invoke or hook functions.
  Powerful, fiddly, risky. ps5debug-NG de-risks this: its `CMD_PROC_CALL`
  (`0xBDAA0006`) calls a function with six register args via a small injected RPC
  stub over the syscall-573 R/W primitive — a concrete recipe to follow (see
  below). *Deferred: advanced.*
- **Install a package** — `sceAppInstUtilAppInstallPkg` plus the installer's
  register-and-poll flow. *Deferred: needs a `.pkg` to build against.*
- **Wake from standby** — a host-side Remote Play magic packet (libSceRemoteplay
  on the console side is only part of it). *Deferred: needs pairing credentials.*
- **Controller output — lightbar colour, rumble, triggers** — `scePadSetLightBar`,
  `scePadSetVibration`, `scePadSetTriggerEffect` all exist, but they need a
  `scePad` handle to the *physical* DualSense, and a background payload cannot
  get one: the system shell (SceShellUI) owns the controller exclusively, so
  `scePadOpen` answers `0x809b0081` (already opened), `scePadGetHandle` returns
  only this process's own (none) `0x80920008`, and `scePadOpenExt2` wants a
  param struct that is not documented (`0x80920001` for a zeroed one). Measured
  on 12.20 with a real controller connected. The virtual pad has no LEDs, so it
  is not a target either. The working path (as in PS5 Suite) is to bridge into
  SceShellUI, which owns the pad session — a separate, larger effort.
  *Deferred: needs a SceShellUI bridge or the ScePadOpenExt2 param layout.*

## Borrowed from ps5debug-NG

[ps5debug-NG](https://github.com/Pharaoh2k/ps5debug-NG) is a GPL-3 debugger
payload (a persistent binary server inside SceShellCore, wire-compatible with
`ps5debug 1.0b5`) covering firmware 3.xx–13.xx, 12.20 included. It is the same
licence as DevLoop, so we can lift code outright; the value is in borrowing its
*techniques* into our one-text-command protocol rather than adopting its wire
format. Ordered roughly by payoff-per-effort.

- **Memory scanning — `scan` / `scan-aob`** *(the standout gap)*. We have
  point-wise `peek`/`poke` but no search. Port their **value scan** (find every
  address equal to a value, then narrow iteratively) and **AOB / byte-pattern
  scan** (`0xBDAA0501`, with a must-match mask). Both are self-contained
  algorithms over our existing memory access — AOB scan is how you locate
  anything in a title's memory, so this is the single biggest force-multiplier
  for game testing. *Effort: medium. Self-contained.*
- **`locate-title` — foreground-app metadata**. Their `0xBDDD0006` returns the
  running game's pid, title id, content id, name and version by scanning the
  proc struct (per-firmware offset fallback) and parsing `param.sfo` on-console.
  Goes beyond our `ps` and removes the lean on a hand-kept `titles.json` for
  "what is running right now". *Effort: low–medium.*
- **`discover` — UDP beacon on 1010**. Reply to a 4-byte `0xFFFFAAAA` datagram
  by echoing it, so a client finds the console on the LAN without a hardcoded
  address — fits our "IP only in `.env`" rule. *Effort: trivial (~20 lines).*
- **Readback-verified writes**. Their `poke` reads the region back and reports a
  mismatch ("truthful writes"). Cheap hardening for our `poke`/`put` that makes
  a remote write trustworthy. *Effort: trivial.*
- **syscall-573 process R/W primitive** (`common/source/sys_proc_rw.c`). An
  alternative to our `mdbg_copyout/copyin`: raise the caller's authid in `ucred`
  (`+0x58` → `0x4800000000000006`), run syscall 573 for the cross-process
  transfer, restore the authid. Reaches any process and underpins their scan,
  call and stack-walk commands. Evaluate as a replacement or fallback for the
  `mdbg` path. *Effort: medium. Foundational.*
- **`klog-stream` — klog forwarder on 3232**. They stream `/dev/klog` over TCP;
  we already replay klog for `klog_evict_cim` and pull it with `kernel-log`, so a
  continuous stream command is an increment on plumbing we have. *Effort: low.*
- **Kernel read / write / base** (`0xBDCC…`). A real kernel-memory tier beyond
  our static probe — base address plus `kern-read`/`kern-write`. Leans on their
  install-time kpatch and SDK kernel primitives, so it is a genuine uplift, not a
  copy-paste. *Effort: high.*
- **Rest-mode survival**. Their server rebinds its sockets after suspend/resume
  and when the network returns — robustness worth having for an always-on
  payload. *Effort: medium.*

Out of scope for DevLoop's automation focus: the full interactive debugger
(attach, 30 breakpoints, 4 watchpoints, thread suspend/resume, register
get/set), the built-in Zydis disassembler and Keystone assembler (heavy
third-party deps), and the SIMD "turbo scan" engine (an optimisation only worth
it once basic scanning exists).

## Notes

- No trophy or save-data stub libraries ship in this SDK, so that automation is
  not a quick win here.
- Memory and process commands run with the payload's elevated credentials.
  `kill`, `poke` and (future) `setting set` can break a running title; use them
  deliberately.
