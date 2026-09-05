import { spawn, spawnSync } from "child_process"
import { log } from "./logger.js"
import type { InsertMethod } from "./types.js"

// Wayland-native typing sink for v4.
//
// dotool drives /dev/uinput against the Xwayland layout, locked to "us" behind
// Hyprland, so Cyrillic can never be typed and the ctrl+shift+u hex hack fails
// in most apps. This sink speaks the Wayland virtual-keyboard via wtype and has
// three insertion methods:
//
//   type (default): wtype types each character with its exact XKB keysym from a
//     keymap that wtype builds itself and uploads with the virtual keyboard, so
//     the target app receives the precise keysym independent of the active
//     layout. No clipboard involved (nothing pollutes clipboard history) and no
//     paste binding involved (works in vim, terminals, tiled editors — anything
//     that accepts keystrokes). Mirrors how nerd-dictation inserts text.
//     Limitation: native-Wayland apps get exact keysyms; XWayland/X11 apps
//     (older Electron in X11 mode, wine, java) interpret the keycodes against
//     their own keymap and can render garbage there.
//
//   dotool: types through /dev/uinput via the dotool server, which many apps
//     (and compositors) treat as a real physical keyboard. English only:
//     keysyms are resolved against the "us" layout, so the result matches the
//     receiver only when the target app is on the (default) US layout. This is
//     an experimental alternative to "type" for apps that mishandle the
//     virtual-keyboard keymap (e.g. Chromium, Electron); it works even in
//     XWayland apps and Google Chrome. Requires `dotool` installed and
//     /dev/uinput access. No compositor/layout-detection dependency.
//
//   paste: wl-copy + wtype paste combo, so non-ASCII lands verbatim everywhere
//     including XWayland. Costs clipboard history and depends on the app's paste
//     binding, which is configurable (`paste`, "ctrl+v" by default).
//
// Accepts the dotool-ish command protocol produced by TypingController:
//   type <text>              -> type keysyms or clip-paste per method
//   key BackSpace [xN]       -> press/backspace
//   (other key chords -> ignored, punctuation handled by the sink already)

export interface DotoolSink {
    write(data: string): void
    readonly writable: boolean
    kill(signal?: NodeJS.Signals): void
}

const DEFAULT_PASTE = "ctrl+v"
const DEFAULT_METHOD: InsertMethod = "type"

// Combo wire format mirrors the settings capture: e.g. "ctrl+v", "ctrl+shift+v",
// "super+v", "ctrl+insert". Modifier aliases map to what wtype accepts.
const MOD_ALIASES: Record<string, string> = {
    ctrl: "ctrl",
    control: "ctrl",
    shift: "shift",
    alt: "alt",
    super: "logo",
    win: "logo",
    logo: "logo",
    meta: "logo",
    altgr: "altgr",
}

function run(cmd: string, args: string[]): void {
    const res = spawnSync(cmd, args, { timeout: 5000 })
    if (res.status !== 0) {
        log("TYPE", `${cmd} ${args.join(" ")} failed status=${res.status} ${res.stderr}`)
    }
}

/** "ctrl+shift+v" -> ["ctrl", "shift"], "v" */
function parsePasteCombo(combo: string): { mods: string[]; key: string } {
    const parts = combo.split("+").map((p) => p.trim()).filter(Boolean)
    if (parts.length === 0) {
        const [cm, ck] = DEFAULT_PASTE.split("+")
        return { mods: MOD_ALIASES[cm] ? [MOD_ALIASES[cm]] : [cm], key: ck }
    }
    const key = parts[parts.length - 1]
    const mods: string[] = []
    for (const p of parts.slice(0, -1)) {
        const alias = MOD_ALIASES[p]
        mods.push(alias ?? p)
    }
    // Single letter keys type as the lowercase letter; the shift modifier (when
    // present) already produces the capital. "V" (from a capturer) must become
    // "v" or wtype would emit V, twice-shifted.
    const normalizedKey = key.length === 1 ? key.toLowerCase() : key
    return { mods, key: normalizedKey }
}

function typeTextViaKeyboard(text: string): void {
    if (!text) return
    // wtype uploads its own keymap mapping each needed character to the exact
    // keysym and presses it, so the layout does not matter for native-Wayland
    // apps. \n inside the text maps to Return via wtype's remap table.
    run("wtype", ["-d", "8", "--", text])
}

function pasteText(text: string, combo: string): void {
    if (!text) return
    run("wl-copy", ["--type", "text/plain;charset=utf-8", "--", text])
    const { mods, key } = parsePasteCombo(combo)
    const args: string[] = ["-s", "100"]
    for (const m of mods) {
        args.push("-M", m)
    }
    args.push("-k", key)
    run("wtype", args)
}

function pressKey(keyName: string): void {
    run("wtype", ["-k", keyName])
}

export interface DotoolSinkOptions {
    pasteCombo?: string
    method?: InsertMethod
    /** xkb layout passed to dotool via DOTOOL_XKB_LAYOUT (e.g. "ru", "us"). */
    dotoolXkbLayout?: string
}

interface DotoolProcSink extends DotoolSink {
    readonly proc: ReturnType<typeof spawn> | null
}

// dotool server channel (types via /dev/uinput). English only: keysyms are
// resolved against the "us" xkb layout, so Cyrillic/non-ASCII are dropped by
// the receiver unless the target app is on the (default) US layout. Mirrors
// nerd-dictation's DOTOOL backend: a persistent `dotool` process fed from stdin
// so keymap/layout state survives between commands. The same binary is
// validated by the preflight and is the daemon behind the `dotoolc` client/fifo
// wrapper. No compositor/layout detection dependency (works on any compositor).
function spawnDotoolProcSink(xkbLayout: string): DotoolProcSink {
    let proc: ReturnType<typeof spawn> | null = null
    try {
        proc = spawn("dotool", [], {
            stdio: ["pipe", "pipe", "pipe"],
            env: { ...process.env, DOTOOL_XKB_LAYOUT: xkbLayout },
        })
        proc.stdin!.write("keydelay 4\nkeyhold 0\ntypedelay 12\ntypehold 0\n")
    } catch (err) {
        log("TYPE", `dotool spawn failed: ${err}`)
        proc = null
    }
    return {
        proc,
        write(data: string) {
            if (!proc) return
            for (const line of data.split("\n")) {
                const t = line.trim()
                if (!t) continue
                if (t.startsWith("type ")) {
                    proc.stdin!.write(`type ${t.slice(5)}\n`)
                } else if (t.startsWith("key ")) {
                    const rest = t.slice(4).trim()
                    if (rest === "BackSpace" || rest.startsWith("BackSpace")) {
                        proc.stdin!.write("key backspace\n")
                    } else if (rest === "enter") {
                        proc.stdin!.write("key enter\n")
                    }
                }
            }
        },
        get writable() {
            return true
        },
        kill() {
            if (proc) proc.kill("SIGTERM")
            proc = null
        },
    }
}

export function spawnDotoolSink(options: DotoolSinkOptions = {}): DotoolSink {
    const pasteCombo = options.pasteCombo ?? DEFAULT_PASTE
    const method = options.method ?? DEFAULT_METHOD

    if (method === "dotool") {
        return spawnDotoolProcSink(options.dotoolXkbLayout ?? "us")
    }

    return {
        write(data: string) {
            // each write() may contain multiple lines
            for (const line of data.split("\n")) {
                const t = line.trim()
                if (!t) continue
                if (t.startsWith("type ")) {
                    const text = t.slice(5)
                    if (method === "paste") {
                        pasteText(text, pasteCombo)
                    } else {
                        typeTextViaKeyboard(text)
                    }
                } else if (t.startsWith("key ")) {
                    const rest = t.slice(4).trim()
                    if (rest === "BackSpace" || rest.startsWith("BackSpace")) {
                        pressKey("BackSpace")
                    } else if (rest === "enter") {
                        pressKey("Return")
                    }
                    // other chords (e.g. ctrl+shift+u) are not needed: text is
                    // typed/pasted by the sink itself
                }
            }
        },
        get writable() {
            return true
        },
        kill() {
            /* nothing persistent to kill */
        },
    }
}
