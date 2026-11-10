# DevLoop capability roadmap

DevLoop has grown from a virtual pad and screen capture into a broad remote
testing and control API for a jailbroken PS5, over the one text-per-connection
protocol on port 9112 (and HTTP on the same port for the dashboard). Each
capability is grounded in a callable SDK function — a stub library, a `ps5/*.h`
kernel helper, or a device ioctl.

Legend: **done** = implemented and verified on firmware 12.20.

## Done

- **Input** — virtual DualSense: `open`/`close`, `press`, `hold`, `release`, `seq`.
- **Sight** — `screenshot` (whole screen), `frame` (a title's own frame),
  `list-captures`/`latest-capture`/`delete-capture`, `kernel-log`, `klog-stream`
  (live).
- **Files** — `ls`, `get`, `put`, `rm`, `mv` (sandbox-escaped), `hash` (SHA-256
  on the console).
- **Processes** — `ps`, `kill`, `locate-title` (the foreground app's ids),
  `module PID NAME` (where a module is mapped), `modules PID` (every loaded
  module), `dlsym PID NAME SYM` (resolve a symbol), `call PID FUNC [ARGS]`
  (invoke a function with up to six integer args, returns RAX, via a ptrace
  thread-hijack), `exec /PATH` (run a console binary and capture its output).
- **Process memory** — `peek`, `poke` (read back and verified), `scan` (find a
  1/2/4/8-byte integer in a span), `scan-aob` (find a byte pattern, `??`
  wildcards).
- **Kernel memory** — `kern-read`, `kern-write` (verified), by absolute address
  (`kernel_copyout`/`kernel_copyin`). `ping` reports the payload's kernel proc
  address as a known-good handle to read.
- **Apps** — `launch` (registered titles), `close-app`, `uninstall`, `install`
  (a `.pkg` already on the console, via `sceAppInstUtilAppInstallPkg`).
- **Browser / net** — `open-url`, `http` (payload-side HTTP GET).
- **Power** — `rest`, `reboot`, `power-off`, `wake` (host-side standby wake
  packet; needs `PS5_WAKE_CREDENTIAL`). The payload's listener rebinds across a
  network drop, so it survives rest/resume.
- **Hardware** — `hwinfo` (model, serial, CPU temp, all 64 SoC sensors + the
  hottest, CPU clock), `df`, `led` (indicator LED via `/dev/icc_indicator`:
  static, `off`, `auto`, `breathe`, `chase`), `fan` (duty read / threshold set
  via `/dev/icc_fan`), `beeper` (`sceKernelIccSetBuzzer*`: beep, `mute`, `vol`).
- **Settings** — `settings` (read the registry as JSON), `setting-set REGID
  VALUE` (write one integer, read back and verified).
- **Notifications** — `notify toast|flyout "MSG"`.
- **Live dashboard** — hosted by the console: `GET /` and `GET /api/hwinfo` on
  the command port, served from an embedded page (gauges, a 64-sensor grid, a
  rolling chart). `tools/ps5web.py` is an optional localhost proxy.
- **Discovery** — a UDP beacon on the command port answers a probe; `discover`
  finds consoles on the LAN by broadcast.
- **Setup** — `doctor`, `ping`, `raw`, `load`.

### Borrowed from ps5debug-NG and PS5-Suite (done)

[ps5debug-NG](https://github.com/Pharaoh2k/ps5debug-NG) and
[PS5-Suite](https://github.com/manos555555/PS5-Suite) are both GPL-3, so code
could be lifted; in practice the *techniques* were ported into our one-text-
command protocol. Landed: memory `scan`/`scan-aob`, `kern-read`/`kern-write`
(verified), readback-verified `poke`, `klog-stream`, the UDP `discover` beacon,
`locate-title`, `module`/`dlsym`, and — from PS5-Suite's live-hardware research —
the console `led`, `fan` and `beeper` (all direct device access, so the `led`
needs no `scePad` handle). Not a planned command, just an available primitive:
the SDK provides `kernel_proc_copyin/copyout` (the syscall-573-style cross-process
access) should `peek`/`poke` ever want it instead of `mdbg`, which they use today.

## Not possible from a background payload (architectural, not a choice)

These three cannot be done from DevLoop's background payload in this environment —
each needs a subsystem the payload fundamentally lacks, not just more code. They
are the honest edge of what this design reaches.

- **DualSense lightbar / rumble / triggers** — `scePadSetLightBar`,
  `scePadSetVibration`, `scePadSetTriggerEffect` need a `scePad` handle to the
  *physical* controller, which SceShellUI owns exclusively (`scePadOpen` →
  `0x809b0081`, measured on 12.20). The only path is to reverse-engineer and
  bridge into SceShellUI's pad session (as PS5-Suite does) — a project in itself.
  *The console's own body LED is controllable via `/dev/icc_indicator` (`led`);
  this item is only the pad's own LEDs/haptics.*
- **Modal dialogs** — `sceMsgDialog` / `sceErrorDialog` / `sceImeDialog` are
  CommonDialog dialogs that draw into the *calling* process's framebuffer and
  must be pumped from its render loop; a background payload has no video context,
  so there is nothing to draw into. (System-drawn `notify` toast/flyout, which do
  not need a render loop, already work.)
- **Live video / framebuffer** — continuous capture to drop the capture-card
  dependency. Reading the *composited* display output from a payload has no
  exposed API (no avcapture stub; `SceAvCapture` is a separate privileged
  service). An unsolved problem, not a missing bit of code.

Out of scope for DevLoop's automation focus: ps5debug-NG's full interactive
debugger (attach, breakpoints, watchpoints, thread/register control), its
built-in Zydis disassembler and Keystone assembler (heavy third-party deps), and
the SIMD "turbo scan" engine (an optimisation only worth it once basic scanning
exists, which it now is).

## Notes

- No trophy or save-data stub libraries ship in this SDK, so that automation is
  not a quick win here.
- Memory, kernel, process and settings commands run with the payload's elevated
  credentials. `kill`, `poke`, `kern-write`, `setting-set`, `fan`, `install`,
  `uninstall`, `exec` and `call` (ptrace attach) can break a running title or
  change console state; use them deliberately. `install`/`exec` are wired and fail gracefully, but a
  positive run needs a real `.pkg` / binary on the console to confirm.
- Writes are read back and verified where it is cheap (`poke`, `kern-write`,
  `setting-set`), so a silent failure is reported as `verify=mismatch`.

## Verification status

- **Exercised on hardware (12.20) this session:** `hwinfo` (64 SoC sensors),
  `df`, `locate-title`, `discover`, `kern-read`, `modules`/`module`, `dlsym`,
  `scan` (found a known value at its exact address), `fan` (duty read), `led`
  (set then `auto`), `ps`, `kill`, the console-hosted dashboard (`GET /`,
  `/api/hwinfo`), `hash` (matches `sha256sum`), and `call` — proved against a
  disposable sleeper process: `call getpid` returned the sleeper's own pid
  (`rax=0x1a0`), `call abs(-5)` returned `0x5`, and the target survived the
  thread-hijack. ptrace is not restricted on 12.20. The earlier capability work
  (`ps`, files, `notify`, `peek`, `http`, power, apps) was verified when it landed.
- **Confirmed by code, not run live** (running them would disturb a live title or
  cut access): write-verify readback for `poke` / `kern-write` / `setting-set`
  (each reports `verify=ok|mismatch`), and rest/resume survival (the accept loop
  re-binds the listener every second until the network returns).
- **Not verified:** `install` / `exec` need a real `.pkg` / binary, and the
  `setting-set` / `kern-write` / `poke` *write* paths were not fired at a target
  to avoid changing console state while a title ran (the readback logic is
  confirmed by code above).
