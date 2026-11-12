# Working with PS5 DevLoop

> In this copy, cut to what testing Playden games uses: no `frame` (a Playden
> game cannot be given the hook), no `launch`, `uninstall` or power commands,
> in the client or in the payload.
> `AGENTS.md` is the guide for the testing itself.

For an agent, or a person, driving a jailbroken PS5 with nobody at the pad. Read
this before the first command; `README.md` says how it works and what was
measured.

## The loop

```bash
tools/ps5loop.py load          # once per console boot (the ELF loader must be up, port 9021)
tools/ps5loop.py open          # the virtual pad; disconnects a real controller (PS reconnects it)
tools/ps5loop.py seq down down cross:300 wait:1000
tools/ps5loop.py screenshot    # the whole screen, about 3 s
tools/ps5loop.py close
```

Look, press, look. Never assume a press landed: a menu's default is not always
the first entry (Quake's "Exit game?" starts on NO), and a title takes a moment
after launch before it reads the pad.

## Controlling the console

The payload also reads the console, not just the pad: `ps`, `locate-title`,
`ls`/`get`/`put`/`rm`/`mv`, `df`, `hwinfo`, `close-app` and `kill` (README.md,
"Controlling the console"). `close-app` with no id closes the foreground app, so
name an id from `ps` unless you mean that one.

## Rules

- **One console job at a time**, and never over a title someone is playing: ask
  the launcher payload first, or look.
- **Do not press Create for less than 1.2 s.** A short press opens the Create
  menu, and no screenshot works until Circle closes it.
- **`open` takes the controller away from the person.** A title reads only its
  user's first controller, so the real one is disconnected. Say so when you
  finish; `open --beside` keeps it, for the home screen only.
- **Leave the console as you found it**: quit the title through its own menu,
  `close` the pad. `screenshot` cleans up after itself;
  `delete-capture PATH` removes a capture properly (file, thumbnail and database
  row) if one is left. Pass the path verbatim; a shell that rewrites absolute
  paths (Git Bash on Windows) will mangle it, so quote or disable that first.
- **Never edit `/system_data/priv/mms/av_content.db`** or delete files under
  `/user/av_contents` by hand: the gallery's database and its files go out of
  step. `delete-capture` asks the system to do it.
- Never print the console's address: it lives in `PS5_HOST` or the ignored `.env`.
