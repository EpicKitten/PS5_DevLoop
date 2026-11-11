# Building and deploying the payload

How to turn `payload/main.c` into `build/ps5loop.elf` and get it running on the
console. `README.md` says what the payload does; `AGENTS.md` says how to drive it
once it is up.

## In short

```bash
tools/embed_dashboard.py     # only if tools/ps5web.html changed
tools/build.sh               # -> build/ps5loop.elf
tools/ps5loop.py raw quit    # only if an older copy is still running
tools/ps5loop.py load        # send it to the ELF loader, wait for its ping
```

## What you need

- The [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) (a prebuilt
  release is enough), with `clang`, `ld.lld` and `make` on the path. The SDK's
  `prospero-clang` wraps the system clang.
- Python 3, for `tools/ps5loop.py` and `tools/embed_dashboard.py`.
- A jailbroken PS5 with an **ELF loader** listening on port 9021.
- The console's address in `PS5_HOST`, or in `.env` (copy `.env.example`; it is
  ignored by git). Never commit or print the address.

## Build

### Linux, or WSL

`tools/build.sh` looks for the SDK in `PS5_PAYLOAD_SDK`, then in
`~/ps5-payload-sdk`, `/usr/local/ps5-payload-sdk` and `/opt/ps5-payload-sdk`. The
folder it wants is the one that holds `bin/prospero-clang` and
`toolchain/prospero.mk`.

```bash
export PS5_PAYLOAD_SDK=~/ps5-payload-sdk
tools/build.sh
```

Arguments go through to `make`, so `tools/build.sh clean` removes the ELF. The
build is one compile of `main.c` with `-Wall -Wextra -Werror`: a warning stops
it.

### macOS

With `PS5_PAYLOAD_SDK` set, the build runs natively as above. Without it, and
with `limactl` installed, `tools/build.sh` runs itself inside a Lima VM that has
the SDK and mounts this folder at the same path. `PS5LOOP_VM` names the VM
(default `ps5`).

### Windows

There is no native build; use WSL. Three things go wrong there:

- **A distribution with only versioned clang** (`clang-18`, `ld.lld-18`, no bare
  `clang`). Make a folder of symlinks under the plain names and put it first on
  `PATH`:

  ```bash
  mkdir -p ~/ps5shim
  for tool in clang clang++ ld.lld llvm-ar llvm-strip; do
      ln -sf "$(command -v $tool-18)" ~/ps5shim/$tool
  done
  export PATH=~/ps5shim:$PATH
  ```

- **A checkout WSL cannot see** (a mapped network drive is not mounted in WSL).
  Copy `payload/main.c`, `payload/dashboard_html.h` and `payload/Makefile` into
  `SOMEWHERE/payload` under `/mnt/c`, run
  `make -C SOMEWHERE/payload` with `PS5_PAYLOAD_SDK` set, and copy
  `SOMEWHERE/build/ps5loop.elf` back to `build/ps5loop.elf` in the checkout. The
  Makefile writes to `../build`, so keep the `payload` folder name.
- **Quoting.** PowerShell and Git Bash both rewrite `$variables` and `/mnt/c`
  paths in an inline `wsl bash -lc "..."`. Put the commands in a script file and
  run `wsl.exe bash /mnt/c/path/to/script.sh`.

### After changing the dashboard

The page the payload serves is compiled in. After editing `tools/ps5web.html`,
regenerate the header, then build, and commit both files:

```bash
tools/embed_dashboard.py     # tools/ps5web.html -> payload/dashboard_html.h
```

To try a page edit without rebuilding, `tools/ps5web.py --html tools/ps5web.html`
serves the local file against the running payload.

## Deploy

The payload is resident: it is loaded once per console boot and stays until
`quit` or a restart.

```bash
tools/ps5loop.py doctor      # the host, the loader and the capture tools
tools/ps5loop.py load        # sends build/ps5loop.elf to port 9021
```

`load` sends the ELF to the loader, then pings the payload on port 9112 for up
to ten seconds and prints its answer. `load PATH` sends another ELF, without the
ping. `PS5_LOADER_PORT` and `PS5LOOP_PORT` change the two ports.

On the console, a notification shows the dashboard's address when the payload
starts.

### Replacing a running copy

A running payload holds port 9112, so a second copy cannot bind it and exits
at once: `load` then prints the **old** copy's ping and looks as if it worked.
Stop the old one first:

```bash
tools/ps5loop.py raw quit    # removes the pads and ends the old payload
tools/ps5loop.py load
tools/ps5loop.py ping        # the pid differs from the one before
```

`quit` closes every virtual pad, so do it when nobody is in the middle of a
title, and press PS on the real controller afterwards to reconnect it.

## When it does not work

| What you see | Cause |
| --- | --- |
| `build: no PS5 payload SDK` | `PS5_PAYLOAD_SDK` does not point at the folder holding `bin/prospero-clang` |
| `clang: not found` from `prospero-clang` | no bare `clang` on the path: the symlink folder above |
| `missing payload: build/ps5loop.elf` | not built yet, or the ELF was not copied back from WSL |
| `load` cannot connect | the ELF loader is not running (it does not survive a reboot), or `PS5_HOST` is wrong |
| `loaded, but the payload does not answer; read klog` | the payload crashed at start: `tools/ps5loop.py kernel-log` once a working copy is up, or your klog server |
| the change is not there after `load` | the old copy is still running: `raw quit`, then `load` |
| the dashboard is the old page | `tools/embed_dashboard.py` was not run before the build |
