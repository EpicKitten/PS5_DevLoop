# Testing Playden games with an agent

For an agent (or a person) testing whether Steam games run under
[Playden](https://github.com/leolobato/playden-ps5) on a jailbroken PS5, with
nobody at the pad. You are the tester: you press the buttons, look at the
pictures, play the game for a few minutes and write down what happened.

This folder is a copy of [PS5 DevLoop](README.md) with one tool added:

- `tools/ps5loop.py`: the pad, the screen, and the console's files, processes
  and logs. It is DevLoop's client with everything else removed (README.md lists
  what), and it decodes screenshots without ImageMagick. `DEVLOOP.md` is its
  own guide.
- `tools/playden.py`: the test around them: what is running, the timed watch,
  Playden's logs, and the report folder with a draft for Playden's test-report
  form.

No capture card, no OCR: every look is `screenshot` (about 3 s), read by you.

## One game, start to finish

```bash
tools/ps5loop.py load                      # once per console boot (ELF loader, port 9021)
tools/playden.py status                    # what answers, what is running, free space
tools/ps5loop.py open                      # the virtual pad; takes the real controller away
tools/playden.py games                     # what Playden has prepared: app, PS5 title, ready?
tools/playden.py begin 620 "Portal 2"      # the report folder; every later command uses it
tools/playden.py launch                    # start it (or press Play in Playden with the pad)
tools/playden.py watch                     # 90 s, nothing pressed: pictures at 30 s and 90 s
#   ... play it: press, look, write it down (below) ...
tools/playden.py close                     # if its own menu has no Quit
#   ... bring Playden back from its Home tile ...
tools/playden.py logs                      # Playden's logs, redacted; fatal lines; the session's end
tools/playden.py verdict works --played gameplay --model "<your model>" \
    --summary "Reaches the menu, new game starts, movement and camera respond."
tools/ps5loop.py close                     # give the pad back; say that PS reconnects the controller
```

Everything lands in `reports/<app>-<name>/`: the screenshots, `run.json` (the
record so far), `<folder>-logs.zip`, `result.json` and `issue.md`.
`tools/playden.py summary` puts every game on one page.

## Look, press, look

```bash
tools/ps5loop.py seq down down cross:300 wait:1000
tools/ps5loop.py screenshot                # -> captures/, to look at
tools/playden.py shot main-menu --note "Options, then Cross: the main menu"   # -> the report, as a step
```

- Never assume a press landed, and never navigate from memory of an earlier
  screen. Playden's UI and a starting game both need a moment after a press.
- A menu's default is not always the first entry. Read the picture.
- `hold` drives sticks and triggers: `ps5loop.py hold --ly 0` walks forward
  until `release`. Release before you take a picture you mean to compare.
- Playden's default controller mode presents an Xbox controller: Cross is A,
  Circle is B, Options is Start.

## Getting a game ready

The user says which games to test. A game must be installed and prepared in
Playden first; `games` lists the ones that are, with the PS5 title Playden
reserved (`FAKE78xxx`).

To install one, drive Playden with the pad, a screenshot after every step:

1. Bring Playden to the front (its tile on the PS5's Home, orange on black).
2. Library, search for the game. Check the page is the right game, by its exact
   name: "Escape Simulator" is not "Escape Simulator 2".
3. Install, on the "Install game" sheet, to console storage. Downloads queue and
   can pause; a paused one is resumed from the Queue row's Actions.
4. Wait until `games` says `ready`. Playden reserves the title when it *starts*
   preparing and finishes a minute or two later. A game started earlier quits in
   two seconds, which says nothing about it; `launch` refuses until then.
5. Optional, and worth it: in the game's settings (Actions > Game settings) turn
   the performance overlay on, detail "Everything". It shows the frame rate in
   games drawn through DXVK (Direct3D 8 to 11). Say so in `verdict --settings`.

A download can take hours. Check `tools/playden.py status` for free space
before a large one, and do not poll: look every few minutes.

## The watch

`watch` is the same first check as playden-auto-tester's: the game must still
be running 90 s after it appears, with nothing pressed.

| It prints | Meaning | Next |
| --- | --- | --- |
| `survived`, screen `moving` or `still` | Running and drawing; still is a menu, a title screen or a freeze | Look at both pictures, then play it |
| `survived`, screen `black` | Runs, shows nothing | Look; press Cross and Options once (a splash may wait for a key); then `logs` for the `vk_shown` and `audible` counters |
| `exited_early` | Quit before 90 s | Look at `screenshot-after-exit.png`, then `logs`: a fatal line makes it a crash |
| `did_not_start` | Never appeared in 45 s | Screenshot: an error dialog? Try once more from Playden's Play button |

The black/still/moving reading compares two screenshots a few seconds apart. It
is a hint; your eyes on the pictures decide.

## Play it

This is what a capture card and a timer cannot do, and why an agent tests.
Spend about five minutes, or 40 presses, per game:

1. **Get past the start.** Splash screens, "Press any button", launchers, a
   first-run language or graphics prompt. A launcher that needs a mouse is a
   finding: write it down.
2. **Reach the main menu**, and move in it. Does the highlight follow the d-pad
   or the stick? `shot main-menu`.
3. **Start a new game.** Skip the intro if it lets you. `shot new-game`.
4. **Play.** Move, turn the camera, use the main action. Take two pictures a
   few seconds apart while holding a stick: does the world move? `shot in-game`.
5. **Pause and quit** through the game's own menu when it has one.

After each stage, `shot` or `note` what you pressed and what you saw: these
lines become the numbered steps in the draft. Write what happened, not what you
hoped ("Cross on New Game: nothing changes after 10 s").

Watch for, and record:

- input that does nothing, or only some buttons working;
- a wrong picture: black areas, missing textures, a picture in a corner, text
  that is unreadable;
- a frame rate that is unplayable (from the overlay, when it shows);
- a freeze: two pictures 5 s apart that are the same during gameplay, and a
  third after a button press that is still the same;
- the game quitting: `tools/playden.py status` shows whether it still runs.

Stop when the game is clearly working, clearly broken, or you are stuck. Being
stuck is a result too, as long as you say where. You cannot hear the game:
sound shows only as the `audible` counter in the log.

## Closing, logs and the result

1. Quit through the game's menu, or `tools/playden.py close`. It closes the
   game by its app id, never whatever happens to be in front.
2. Bring Playden back from its Home tile. Playden copies a game's log only when
   Playden next starts, so `logs` taken earlier holds the previous run (it
   warns when the session is one it has seen).
3. `tools/playden.py logs`. Read what it prints: fatal lines, Wine's `err:`
   channels, the session's last lines.
4. `tools/playden.py verdict RESULT ...`.

| Result | When |
| --- | --- |
| `works` | It reached a menu or gameplay with a correct picture, and kept running. Say how far you got with `--played menu` or `--played gameplay` |
| `broken` | It starts, but cannot be played: black screen while the log shows frames or sound, input that never responds, a freeze or a crash on the way into the game, a picture too wrong to play |
| `no-start` | It quit or crashed before showing anything, never appeared, or sat on a black screen or a spinner with no sign of drawing |
| `refused` | Playden would not prepare it. Say whether "Try anyway" was tried |
| `untested` | You went wrong, not the game: a screen you could not get past in Playden, a launch you could not make. No draft is written |

`--summary` is one or two plain sentences that name what you saw. `--model` is
your own model name: the draft says the report is AI generated and by what.

Run a game a second time before calling it `no-start` or `broken` on one odd
run; `begin` again keeps the folder and adds to it.

## Rules

- **One console job at a time, and never over someone playing.** `status` first;
  if a title is running that you did not start, stop and ask.
- **Never read `/data/playden/user`.** `session.v1` there is the Steam sign-in.
  `playden.py` refuses it; do not reach it with `ps5loop.py get` or `ls` either.
- **Nothing is posted.** `issue.md` is a draft for the user to review; its
  Checks are left unticked for them. Do not open issues or comment on Playden's
  tracker.
- **Logs are redacted** (Steam account name, SteamIDs) by `logs`. Do not copy
  raw log files into a report by another route.
- **Uninstall only when asked**, through Playden, and keep the saves: "Delete
  saves too" stays Off. Check the dialog names the exact game first.
- **Do not press Create for less than 1.2 s.** A short press opens the Create
  menu, and no screenshot works until Circle closes it.
- **`open` takes the controller away from the person.** Say so when you finish,
  and `close` the pad: PS on the real controller reconnects it.
- **No power commands, and no PS5 uninstall.** They were removed on purpose,
  from the payload as well as the client: a reboot loses the jailbreak until
  the user loads it again. `kill` and `rm` are for a stuck game and your own
  files only.
- Never print the console's address: it lives in `PS5_HOST` or the ignored
  `.env`.
- Paths are the console's. A shell that rewrites absolute paths (Git Bash on
  Windows) mangles them: use PowerShell, or `MSYS_NO_PATHCONV=1`.

## Not yet proven on a console

`playden.py` was written with the console off. Its parsing, redaction, screen
measure and drafts are covered by `tests/test_playden.py`; these three were
taken from the payload's source and from playden-auto-tester, and need one
look on the first real run:

- "Running" for a game. On firmware 12.20 `ps` prints only a title's number
  (`77031` for `FAKE77031`), so a title counts as running when its sandbox
  folder exists *and* a process carries its number. Seen with Playden itself
  in front; not yet with one of its games.
- `screenshot` (the Create hold) working while a Playden game is in front.
- `launch`: Playden's relaunch helper sent through the ELF loader. Pressing
  Play in Playden with the pad always works as the other way in.

Fix the tool, and this section, when one of them turns out different.

## Changing the tools

`python -m unittest discover -s tests` before a commit. Standard library only,
plus Pillow for the screen measure and for scaling pictures.

The payload (`payload/main.c`) is DevLoop's with the commands a game test does
not use taken out; `BUILDING.md` says how to build it. After a rebuild, send
`tools/ps5loop.py raw quit` before `load`: an old copy holds the port and the
new one exits without a word.
