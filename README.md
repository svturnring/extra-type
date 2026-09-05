# extra-type

Dictation for Linux Wayland: press a hotkey, speak, press it again — the text appears where you are typing.

extra-type is a reworked version of the 4th (Chromium-based) version of [voice-type](https://github.com/eriknovikov/voice-type). It is built specifically for Wayland.

It is designed for modest hardware. Unlike self-hosted models such as Whisper, extra-type does almost nothing locally: speech recognition runs inside Chrome (Google Web Speech API), so it works comfortably even on weak machines.

## What you get

- **Visual feedback** — a spectrum pill and a live transcript overlay shown while you dictate.
- **Several ways to get the text** — typed straight into the app (no clipboard, independent of the keyboard layout), pasted through the clipboard, collected in the overlay and copied when you stop, or typed through uinput (experimental, English only, works in Chrome/XWayland). Choose the mode that suits the app you are typing into.
- **Reliable error handling** — recognition sessions are restarted automatically when they break, and errors surface in the overlay instead of silently dying.
- **CLI and tray** — toggle from the terminal (`extra-type toggle`) or from a tray icon, plus a global hotkey and a desktop launcher.

## Requirements

A Wayland session and Chrome or Chromium. The installer can fetch everything else automatically.

## Install

```bash
git clone https://github.com/svturnring/extra-type.git
cd extra-type
sh install.sh
```

The installer builds the daemon and visualizer and asks whether to install the dependencies automatically.

## Usage

```bash
extra-type          # run the daemon
extra-type toggle   # start / stop dictation
extra-type status   # show what is running
extra-type settings # open the settings window
extra-type stop     # stop the daemon
```

Turn dictation on, speak, turn it off — the text is already in your app. Tune everything (language, hotkey, insertion mode, sounds) in `extra-type settings` or directly in `~/.config/extra-type.jsonc`.

## Notes

Live typing works correctly only in native Wayland apps. In Chrome and Electron apps (Discord and others) it can garble or erase the dictated text: unlike a terminal, the browser translates virtual-keyboard keycodes by its own active layout, so wtype's exact keysyms do not land. For reliable input in these apps, turn **Live typing off** and use **Paste via clipboard**, or use the **Overlay** mode instead.

There is also an experimental **Dotool (uinput)** insertion mode for such apps — it types through `/dev/uinput` like a real keyboard, so it works even in XWayland apps and Google Chrome. It is **English only**: it resolves keysyms against the US layout, so the target app must be on the US/English layout for correct output. Selecting it forces the dictation language to English.

## Update

```bash
extra-type update
```

## Uninstall

```bash
sh uninstall.sh
```

## License

MIT — see [LICENSE](LICENSE).