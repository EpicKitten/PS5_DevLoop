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
  `module PID NAME` (where a module is mapped), `dlsym PID NAME SYM` (resolve a
  symbol).
- **Process memory** — `peek`, `poke` (read back and verified), `scan` (find a
  1/2/4/8-byte integer in a span), `scan-aob` (find a byte pattern, `??`
  wildcards).
- **Kernel memory** — `kern-read`, `kern-write` (verified), by absolute address
  (`kernel_copyout`/`kernel_copyin`). `ping` reports the payload's kernel proc
  address as a known-good handle to read.
- **Apps** — `launch` (registered titles), `close-app`, `uninstall`.
- **Browser / net** — `open-url`, `http` (payload-side HTTP GET).
- **Power** — `rest`, `reboot`, `power-off`.
- **Hardware** — `hwinfo` (model, serial, CPU temp, all 64 SoC sensors + the
  hottest, CPU clock), `df`, `led` (indicator LED via `/dev/icc_indicator`),
  `fan` (duty read / threshold set via `/dev/icc_fan`), `beeper`
  (`sceKernelIccSetBuzzer`).
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
needs no `scePad` handle). The SDK already provides `kernel_proc_copyin/copyout`
(the syscall-573-style cross-process primitive) should `peek`/`poke` ever want
it instead of `mdbg`.

## Still blocked (needs a resource or a larger effort)

- **DualSense lightbar / rumble / triggers** — `scePadSetLightBar`,
  `scePadSetVibration`, `scePadSetTriggerEffect` need a `scePad` handle to the
  *physical* controller, which SceShellUI owns exclusively (`scePadOpen` →
  `0x809b0081`, measured on 12.20). The working path is a SceShellUI bridge (as
  PS5-Suite does) — a separate, larger effort. *The console's own body LED is now
  controllable via `/dev/icc_indicator` (`led`); this item is only the pad.*
- **Call a function in a running process** — the *resolve* half is done
  (`dlsym`); actually invoking a function with arguments needs a small RPC stub
  injected and jumped to (ps5debug-NG's `CMD_PROC_CALL` recipe). *Needs: an
  injected stub; advanced.*
- **Modal dialogs** — `sceMsgDialog` / `sceErrorDialog` / `sceImeDialog` are
  CommonDialog dialogs that draw into the *calling* process's framebuffer and
  must be pumped from its render loop; a background payload has no video context.
  *Needs: a title's render loop, or an injected context.* (System-drawn `notify`
  toast/flyout already work.)
- **Live video / framebuffer** — continuous capture to drop the capture-card
  dependency. Reading the *composited* output from a payload is the known hard
  problem (no avcapture stub; `SceAvCapture` is a separate service). *Needs:
  research.*
- **Install a package** — `sceAppInstUtilAppInstallPkg` plus the register-and-
  poll flow. *Needs: a `.pkg` to build and verify against.*
- **Wake from standby** — a host-side Remote Play magic packet. *Needs: pairing
  credentials.*
- **Rest-mode survival** — rebinding the sockets across suspend/resume. Hard to
  test (resting ends access) and the loader usually relaunches the payload on
  resume anyway. *Not implemented; low priority.*

Out of scope for DevLoop's automation focus: ps5debug-NG's full interactive
debugger (attach, breakpoints, watchpoints, thread/register control), its
built-in Zydis disassembler and Keystone assembler (heavy third-party deps), and
the SIMD "turbo scan" engine (an optimisation only worth it once basic scanning
exists, which it now is).

## Notes

- No trophy or save-data stub libraries ship in this SDK, so that automation is
  not a quick win here.
- Memory, kernel, process and settings commands run with the payload's elevated
  credentials. `kill`, `poke`, `kern-write`, `setting-set`, `fan` and `led` can
  break a running title or change console state; use them deliberately.
- Writes are read back and verified where it is cheap (`poke`, `kern-write`,
  `setting-set`), so a silent failure is reported as `verify=mismatch`.
