# AGENTS.md — extra-type

A system-wide speech-to-text daemon for Linux/Wayland.

## Architecture

### Daemon (`src/`, TypeScript/Bun)

- `index.ts` — CLI entry (`extra-type`, `extra-type toggle|status|stop|start|help|settings|lang|autodetect|update`).
- `daemon.ts` — HTTP daemon on 127.0.0.1:3232. Routes: `/health`, `/status`, `/start`, `/stop`, `/toggle`, `/exit`, `/togglePunctuation`, `/setlang`, `/config` (GET+POST), `/pill/clear`, `/open-settings`.
- `config.ts`, `types.ts` — config load/validate and `ExtraTypeConfig` type (includes `autodetect_lang`, `visualizer`).
- `visualizer.ts` — spawns/kills `extra-type-viz` (detached).
- `vizState.ts` — daemon→viz handoff via `~/.local/state/extra-type/viz-state.json`.
- `browserLauncher.ts`, `browser.js`, `browserRecognition.js` — headless Chrome via puppeteer-core. The page pushes lifecycle events to the daemon
  through exposed functions (`onSpeechEvent`, `onBrowserRecStart/Stop/Error/Restart`); `onBrowserRecStart` fires on the WSA `onstart` callback and is the
  authoritative "dictation is live" signal.
- `layoutWatcher.ts` — pluggable layout detection chain (replaces hardcoded hyprctl).
- `layoutProviders/` — chain: hypr → sway → xkb → fallback. Each provider is optional.
- `tray.ts` — StatusNotifierItem (SNI) via `dbus-next` + DBusMenu (toggle, settings, quit).
- `dotoolSink.ts`, `typingController.ts` — text insertion. Default `insert_method: "type"` types each character
  as an exact XKB keysym from a keymap wtype builds/uploads itself (layout-independent for native-Wayland apps, no clipboard).
  `"paste"` does wl-copy + wtype paste combo (works in XWayland apps, costs clipboard history + a paste binding).
  `"dotool"` types through `/dev/uinput` via the `dotool` server (English only, works in Chrome/XWayland).
- `speechEventGate.ts`, `speechPipeline.ts`, `transcriptTransformers/` — speech event handling.
- `hotkey.ts` — spawns/kills `extra-type-hotkey` (evdev global hotkey helper) when config `hotkey` is set.
- `viz/hotkey.c` — pure-libc helper: grabs keyboards at evdev layer, passes keys through via /dev/uinput, swallows the combo key and POSTs `/toggle`. No external deps (kernel headers only). Works together with keyd by grabbing keyd's virtual keyboard when the physical ones are busy.

### Visualizer (`viz/`)

- `spectrum.c` — GTK4 + gtk4-layer-shell + PipeWire FFT spectrum visualizer; also hosts the transcript
  overlay (pill mode: text area + Copy/Clear buttons, HTTP to the daemon via `--port`).
- `settingsWindow.c` — GTK4 settings window (connects to daemon HTTP API).

### Build

From the repo root:
- `make all` — builds daemon + viz + settings window.
- `make install` — installs to `/usr/local/bin/`, plus a `Extra Type` `.desktop`
  launcher (`/usr/local/share/applications/extra-type.desktop`, opens the settings
  window) and an SVG icon (hicolor scalable apps). Template: `extra-type.desktop.in`.
- `make clean` — cleans build artifacts.

Or single-command install: `sh install.sh`

### Config

`~/.config/extra-type.jsonc` — JSON with comments. Key fields:
- `port` (int, default 3232)
- `lang` (BCP47, default "en-US")
- `autodetect_lang` (bool, default false)
- `hotkey` (string|null, default null) — compositor-independent global toggle combo, e.g. `"ctrl+alt+space"` (mods+evdev code). Set/clear via settings window or config.
- `browser_type` ("chrome"|"chromium"), `browser_path` (string)
- `stream` (bool, default true) — live interim transcripts (typed/edit in real time; also drives the pill overlay in `insert_method: "pill"`)
- `timeout` (int seconds, default 0) — silence auto-stop
- `sound` (bool, default true) — embedded start/stop sounds + system sounds for errors
- `punctuation` (bool) — spoken punctuation for English
- `insert_method` ("type"|"paste"|"pill"|"dotool", default "type") — "type" types chars as exact keysyms via wtype (no clipboard, works in any
  app accepting keystrokes; layout-independent for native-Wayland apps); "paste" uses wl-copy + a paste combo (XWayland-safe,
  costs clipboard history, depends on the app's paste binding); "pill" types nothing — the transcript collects into the
  visualizer overlay (works in any app, incl. XWayland) and is copied out with the overlay buttons; "dotool" types via
  `/dev/uinput` through the `dotool` server (English only; forces lang to en-US, for apps that mishandle wtype)
- `paste` (string, default "ctrl+v") — the paste combo used when `insert_method: "paste"`, e.g. `"ctrl+shift+v"` (mods+keyname)
- `visualizer: { enabled, path }`

### Runtime deps

- Chrome/Chromium (Web Speech API)
- wtype (char typing via Wayland virtual keyboard); wl-copy only when `insert_method: "paste"`
- dotool only when `insert_method: "dotool"` (needs `/dev/uinput` access, `input` group)
- GTK4, gtk4-layer-shell, PipeWire, libsoup3, json-glib (viz + settings window)
- dbus-next (tray icon)

### Layout detection

Pluggable chain in `layoutProviders/`:
1. `hyprctl -j devices` → Hyprland
2. `swaymsg -t get_inputs` → Sway
3. `setxkbmap -query` → Xwayland/X11
4. Fallback → `lang` from config

Auto-detect is **off by default** (`autodetect_lang: false`). Enable via settings window or `extra-type autodetect on`.
Works only where a provider answers: Hyprland (`hyprctl`), Sway (`swaymsg`), X11/Xwayland (`setxkbmap`). Other compositors
(e.g. niri, pure Wayland) fall through to the configured `lang`.

### Text insertion

**Three methods (`insert_method`), plus a collect mode — selectable in the settings window:**

- **`type` (default)** — wtype types each character as the exact XKB keysym. wtype uploads its own keymap
  (every needed char gets its own key → keysym) through `zwp_virtual_keyboard_v1`, so native-Wayland apps receive the
  precise character regardless of the active layout — no clipboard (clipboard history untouched), no paste binding
  (works in vim, terminals, anything that accepts keystrokes). Same mechanism nerd-dictation uses.
  Limitation: XWayland/X11 apps interpret keycodes against their own keymap and can render garbage — use `paste`, `pill`
  or `dotool` there.
- **`paste`** — wl-copy + wtype pressing a configurable paste combo (`paste`, default `ctrl+v`). Verbatim text lands
  everywhere including XWayland, at the cost of clipboard history and a working paste binding in the target app.
- **`pill`** — nothing is typed. The transcript collects into the visualizer overlay (a dark, rounded, bordered text area shown
  instead of the plain spectrum pill, visible only while dictating). Segments are appended verbatim — the API already provides
  the needed spaces. On stop, the full transcript is copied to the clipboard automatically (`wl-copy`) and the overlay resets,
  with a caption hinting to turn off dictation to copy. Works in every app including X11/XWayland — nothing is injected into it.
  The daemon pushes the text into viz-state (`dictationText`/`pill`).
- **`dotool`** — types through `/dev/uinput` via the `dotool` server (spawned directly on stdin, as nerd-dictation's
  DOTOOL backend — not via the `dotoolc` client). Apps treat it as a real physical keyboard, so it works in Chrome,
  Electron and XWayland apps where wtype's virtual-keyboard keymap is mishandled. **English only**: keysyms are resolved
  against the `us` layout, so the target app must be on the US layout; selecting it forces `lang` to `en-US` and disables
  layout auto-detect. Requires `dotool` installed and `/dev/uinput` access.

Backspace/Enter are always wtype keysyms (`BackSpace`, `Return`) for the native-wtype backends; the dotool sink
maps them to dotool's own key names (`key backspace`, `key enter`), independent of the method. The settings window
captures the paste combo with the same key-grabber as the hotkey but stores named keys (`ctrl+shift+v`), not evdev codes.

### Hotkey helper

- `extra-type-hotkey` grabs keyboards at the evdev layer (EVIOCGRAB, pure libc),
  forwards every key verbatim through a `/dev/uinput` virtual keyboard, and
  swallows only the configured combo, POSTing `/toggle` to the daemon. Repeats
  (value==2) are never triggers; a 400ms helper debounce layers on the daemon's
  800ms toggle debounce.
- If `keyd` is installed it already holds the physical keyboards (grab returns
  EBUSY) and re-emits through its own virtual keyboard; the helper skips busy
  devices and grabs the keyd virtual keyboard instead, so both coexist.
- Requires read/write on `/dev/input/event*` and `/dev/uinput` (the `input`
  group is enough). The helper self-exits after 3 failed `/health` checks so a
  killed daemon cannot leave a grabbed keyboard behind.
- Combo wire format: `mods+evdevcode`, e.g. `ctrl+shift+47`. Settings window
  captures keys via a GTK event controller and maps the platform keycode to the
  evdev code (-8 on X11).
- Settings window applies every change immediately (no Apply button); the
  status label shows errors only. Global hotkey row uses a capture-key dialog
  and a Clear button.

### Watchdog

Recognition state is **push-based**: the browser fires `onBrowserRecStart` on the WSA `onstart`
callback and the daemon only then turns the pill green (`status:"listening"`). Start, browser
recovery, and live language switch keep the pill amber (`starting`/`recovering`) until that
confirmation, with an 8 s timeout guard that flips to red (`error`) if recognition never starts.

The daemon polls browser health every 5 s while listening (`watchdogTick`) purely as a backstop
for silent stalls (WSA stops producing callbacks with no `onend`/`onerror`). If the page dies or
recognition stops, it reinitializes the browser, shows `loading`/`error` in the pill and resumes
recognition.

### Sounds

- `sound: true` → embedded start/stop MP3s on recording start and stop (extracted from the binary to
  `tmpdir()` as `extra-type-start.mp3`/`extra-type-stop.mp3`, played via mpv/ffplay/pw-play, or canberra `-f`),
  freedesktop system sounds (`dialog-error`) for offline/error.
- Daemon start/stop itself plays no sounds — only text notifications (D-Bus, always on; the settings toggle was removed).
- The start/stop sound files are compiled into the daemon (`src/soundsData.ts`); user config has no `sound_start`/`sound_stop`.

### Caveats

- LSP reports false positives in `.c` files (missing pkg-config paths). `gcc` compiles cleanly.
- Never run long/verifying commands in foreground — use `nohup setsid … & disown`.
- Kill by PID, not `pkill -f` (kills invoking shell).
