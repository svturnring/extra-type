# Security

extra-type is a single-user, localhost-only dictation daemon. This page
describes what it touches and what the attack surface looks like.

## Permissions

### Microphone Access

- **Purpose**: Capture your voice for cloud speech recognition.
- **When active**: The microphone is read only during an explicit dictation
  session started by you (hotkey/API/tray).
- **Data flow**: PCM audio is captured by a headless Chrome tab running the
  Web Speech API and sent by the browser directly to Google's speech service.
  The daemon itself does not record or upload audio.
- **Local retention**: On recognition errors, diagnostic audio may be kept under
  `$TMPDIR/extra-type/` so it is not silently lost.

### Text Insertion

- **Default (`type`)**: the daemon runs `wtype`, which uploads its own XKB
  keymap to a Wayland virtual keyboard and pressess exact keysyms. No clipboard
  is touched.
- **Paste mode**: `wl-copy` puts the transcript on the CLIPBOARD selection for
  a moment, then `wtype` presses the paste combo. While that is in flight the
  clipboard is briefly readable by any app — if your clipboard held a secret it
  is replaced until another copy happens. PRIMARY is never touched.
- **Pill mode**: nothing is inserted anywhere; the transcript collects in the
  visualizer overlay and is copied to the clipboard only when you stop
  dictation or press the overlay button.

### `/dev/uinput` (only for the optional global hotkey helper)

- The `extra-type-hotkey` helper grabs keyboards at the evdev layer and re-emits
  them through `/dev/uinput`, swallowing only the configured combo. It is only
  started when a `hotkey` is configured. Needs write access to
  `/dev/input/event*` and `/dev/uinput` (the `input` group is enough).

## Network Access

- **Recognition**: audio is sent by Chrome to Google's Web Speech API endpoint.
- **API**: the daemon serves an HTTP API bound to `127.0.0.1` only, never the
  network, and has no authentication (single-user desktop design).
- **Self-update**: `extra-type update` talks to the GitHub Releases API of the
  `svturnring/extra-type` repo.
- **No telemetry**: extra-type collects no usage analytics and sends nothing
  unrelated to the above.

## Filesystem Access

- **Configuration**: reads/writes `~/.config/extra-type.jsonc` (respects
  `XDG_CONFIG_HOME`). The pre-rename `voice-type.jsonc` is migrated on first run.
- **State**: daemon ↔ visualizer handoff in `~/.local/state/extra-type/`.
- **Logs**: `~/.local/share/extra-type/logs/` (respects `XDG_STATE_HOME` /
  `XDG_DATA_HOME`). Config values such as the browser profile are not secrets.

## What extra-type Does Not Do

- Does not record audio when no dictation session is active.
- Does not expose the API beyond `127.0.0.1`.
- Does not type through `/dev/uinput` on X11/XWayland targets that it cannot
  render (char typing is limited to native-Wayland apps; see README).
- Does not collect telemetry or usage statistics.

## Reporting a Vulnerability

Open a private issue or security advisory in the
[repository](https://github.com/svturnring/extra-type/security).