# tpgest

A small macOS trackpad gesture recognizer in one C file. It reads raw finger contacts from Apple's private `MultitouchSupport` framework, recognizes swipes, pinches and double taps by finger count, and fires keyboard shortcuts or puts the Mac to sleep.

Default bindings (edit `bindings[]` in `tpgest.c`):

| Gesture | Action |
|---|---|
| 4-finger swipe up | ⌘T |
| 4-finger swipe down | ⌘W |
| 3-finger pinch in | ⌃⌘F |
| 5-finger double tap | sleep |

## Build

```sh
make
```

Needs the Xcode Command Line Tools. No other dependencies.

## Try it

```sh
./tpgest --dry-run --debug
```

`--dry-run` logs recognized gestures without firing anything. `--debug` also prints a line per finger-count phase (duration, travel, spread ratio), which is what you tune from.

## Run at login

1. Install and start it as a LaunchAgent pointing at this checkout's binary:

   ```sh
   make install
   ```

2. Add the `tpgest` binary under System Settings → Privacy & Security → Accessibility so it can send keystrokes.

Logs go to `~/Library/Logs/tpgest.log`. Stop and remove it with `make uninstall`.

Rebuilding the binary changes its signature, so macOS drops the Accessibility grant. Remove and re-add it after each rebuild.

## Tuning

These can be passed as arguments (add them to `ProgramArguments` in `~/Library/LaunchAgents/tpgest.plist`, then `launchctl kickstart -k gui/$(id -u)/tpgest`), so tuning doesn't need a rebuild:

| Flag | Default | Meaning |
|---|---|---|
| `--settle=SEC` | 0.03 | finger count must hold this long before a gesture fires |
| `--swipe=DIST` | 0.08 | travel (fraction of the trackpad) to count as a swipe |
| `--pinch-in=RATIO` | 0.80 | spread must shrink to this fraction to count as a pinch |

## Caveats

- `MultitouchSupport` is undocumented and may break with a macOS update.
- Trackpads can go silent after sleep, so tpgest restarts itself (re-execs) a few seconds after each wake. Connecting a Magic Trackpad while awake still needs a restart (`launchctl kickstart -k gui/$(id -u)/tpgest`).
- Turn off any macOS trackpad gesture that uses the same fingers and direction (System Settings → Trackpad → More Gestures), or both will fire.
