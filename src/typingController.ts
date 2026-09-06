import type { DotoolKeyChord } from "./transcriptTransformers/types.js"
import type { DotoolSink } from "./dotoolSink.js"
import { spawnDotoolSink } from "./dotoolSink.js"
import type { InsertMethod } from "./types.js"
import { log } from "./logger.js"

// Wayland-friendly replacement.
//
// The stock v4 used dotool + a ctrl+shift+u hex hack for any non-ASCII char,
// which only works in GTK apps and never with a Cyrillic layout hidden behind
// Xwayland ("us" only). Our sink speaks wtype with three methods:
//   - "type" (default): types text as exact XKB keysyms via wtype's own
//     uploaded keymap — layout-independent for native-Wayland apps, no
//     clipboard, no paste binding (works in vim & terminals).
//   - "paste": clipboard paste (wl-copy + wtype combo) so non-ASCII also lands
//     verbatim in XWayland apps.
//   - "pill": never types anything. The transcript is collected and surfaced
//     through an onPillText listener (the daemon pushes it into the visualizer
//     overlay so the user can copy it where they want). Works with any app —
//     including X11/XWayland ones — because nothing is injected into them.
// Newlines map to Enter (a raw "\n" would break the line-based script protocol
// downstream).
export default class TypingController {
    private prevText: string = ""
    private accFinal: string = ""
    private onPillText: ((text: string) => void) | null = null
    private dotool: DotoolSink
    private injectedSink: boolean
    private pasteCombo: string
    private insertMethod: InsertMethod
    public hasStopped = false

    constructor(sink?: DotoolSink, pasteCombo: string = "ctrl+v", insertMethod: InsertMethod = "type") {
        this.pasteCombo = pasteCombo
        this.insertMethod = insertMethod
        this.injectedSink = sink !== undefined
        this.dotool = sink ?? spawnDotoolSink({ pasteCombo, method: sinkMethod(insertMethod) })
    }

    /** Receive the collected transcript in "pill" mode (daemon → visualizer). */
    public setPillListener(listener: (text: string) => void): void {
        this.onPillText = listener
    }

    /** Swap the paste key combo live (settings change while daemon runs). */
    public setPasteCombo(combo: string) {
        if (!combo || combo === this.pasteCombo) return
        this.pasteCombo = combo
        if (!this.injectedSink) this.rebuildSink()
    }

    /** Swap insertion method live ("type"/"paste" keysyms vs "pill" collect). */
    public setInsertMethod(method: InsertMethod) {
        if (method !== "type" && method !== "paste" && method !== "pill" && method !== "dotool") return
        if (method === this.insertMethod) return
        this.insertMethod = method
        this.prevText = ""
        this.accFinal = ""
        if (!this.injectedSink) this.rebuildSink()
        if (method === "pill" && this.onPillText) this.onPillText("")
    }

    /** Drop the collected pill text (POST /pill/clear from the visualizer). */
    public clearPill() {
        this.prevText = ""
        this.accFinal = ""
        if (this.onPillText) this.onPillText("")
    }

    /**
     * Call AFTER a browser / WSA restart while still listening. The Web Speech
     * session was interrupted, so the previously-typed interim text on screen
     * can no longer be edited by diffing against new partials — otherwise the
     * first partial after the restart would backspace over it all. Keep what is
     * already on screen (it is already typed), stop tracking it as editable,
     * and for pill mode fold the interrupted interim into the finalized
     * transcript so nothing is lost.
     */
    public recoverRestart() {
        if (this.insertMethod === "pill" && this.prevText) {
            this.accFinal += this.prevText
            if (this.onPillText) this.onPillText(this.pillText())
        }
        this.prevText = ""
    }

    /** Replace the pill transcript from the overlay (user edits in the viz). */
    public setPillText(text: string) {
        if (this.insertMethod !== "pill") return
        this.accFinal = text
        this.prevText = ""
        if (this.onPillText) this.onPillText(text)
    }

    private rebuildSink() {
        const old = this.dotool
        this.dotool = spawnDotoolSink({ pasteCombo: this.pasteCombo, method: sinkMethod(this.insertMethod) })
        old.kill("SIGTERM")
    }

    private isPill(): boolean {
        return this.insertMethod === "pill" && !this.hasStopped
    }

    public sendBackspaces(count: number) {
        if (count <= 0 || this.isPill()) return
        log("TYPING", `backspace x${count}`)
        this.dotool.write("key BackSpace \n".repeat(count))
    }

    public sendKeyChord(chord: DotoolKeyChord) {
        if (this.hasStopped || this.insertMethod === "pill") return
        log("TYPING", `key ${chord}`)
        this.dotool.write(`key ${chord}\n`)
    }

    public typeText(text: string) {
        if (this.hasStopped || this.insertMethod === "pill") return
        if (!text) return

        let script = ""
        let chunk = ""

        const flushChunk = () => {
            if (chunk.length > 0) {
                script += `type ${chunk}\n`
                chunk = ""
            }
        }

        for (const char of text) {
            if (char === "\n") {
                flushChunk()
                script += `key enter\n`
            } else {
                chunk += char
            }
        }
        flushChunk()

        if (script.length > 0) {
            log("TYPING", `text ${JSON.stringify(text)}`)
            this.dotool.write(script)
        }
    }

    public applyLiveText(currText: string) {
        if (this.hasStopped) return
        if (currText === this.prevText) return

        /* pill mode: nothing is typed, the full transcript is pushed to the listener */
        if (this.insertMethod === "pill") {
            this.prevText = currText
            if (this.onPillText) this.onPillText(this.pillText())
            return
        }

        if (this.prevText === "") {
            this.typeText(currText)
        } else {
            const commonPrefixLen = findCommonPrefixLen(currText, this.prevText)
            const charsToDelete = this.prevText.length - commonPrefixLen
            const charsToAdd = currText.slice(commonPrefixLen)
            this.sendBackspaces(charsToDelete)
            this.typeText(charsToAdd)
        }
        this.prevText = currText
    }

    public finalizeSegment() {
        if (this.insertMethod === "pill" && this.prevText !== "") {
            /* the transcript from the API already carries the needed spaces —
             * append verbatim, never force separators between segments */
            this.accFinal += this.prevText
            this.prevText = ""
            if (this.onPillText) this.onPillText(this.pillText())
            return
        }
        this.prevText = ""
    }

    /** Full pill transcript: finalized segments + current interim. */
    public getPillText(): string {
        return this.pillText()
    }

    private pillText(): string {
        return (this.accFinal + this.prevText).trimEnd()
    }

    public reset() {
        this.prevText = ""
        if (this.insertMethod === "pill") {
            this.accFinal = ""
            if (this.onPillText) this.onPillText("")
        }
    }

    public destroy() {
        this.dotool.kill("SIGTERM")
    }
}

function sinkMethod(method: InsertMethod): "type" | "paste" | "dotool" {
    if (method === "pill") return "type"
    return method
}

function findCommonPrefixLen(currText: string, prevText: string) {
    let i = 0
    while (currText[i] === prevText[i]) i++
    return i
}