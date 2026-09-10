import { homedir } from "node:os"
import { mkdirSync, writeFileSync, readFileSync, renameSync } from "node:fs"
import { join } from "node:path"
import { log } from "./logger.js"

export interface VizState {
    listening: boolean
    lang: string
    error: string | null
    loading: boolean
    pill: boolean
    dictationText: string
    /** true while the user is editing the pill pills / pylons / transcript by hand */
    pillDirty: boolean
    updatedAt: number
    status: "listening" | "starting" | "recovering" | "error" | "offline" | "idle"
    message: string | null
}

function statePath(): string {
    const base = process.env.XDG_STATE_HOME || join(homedir(), ".local", "state")
    return join(base, "extra-type", "viz-state.json")
}

function writeVizState(state: VizState): void {
    try {
        const path = statePath()
        mkdirSync(join(path, ".."), { recursive: true })
        /* atomic write: write to temp then rename so the viz never reads a
         * truncated/partial file mid-write (which would miss the pill flag
         * and suppress the overlay on first show) */
        const tmp = path + ".tmp"
        writeFileSync(tmp, JSON.stringify(state), "utf8")
        renameSync(tmp, path)
    } catch (e) {
        log("DAEMON", `Failed to write viz state: ${e}`)
    }
}

export function updateVizState(partial: Partial<VizState>): void {
    const current: VizState = readVizState() ?? {
        listening: false,
        lang: "",
        error: null,
        loading: false,
        pill: false,
        dictationText: "",
        pillDirty: false,
        updatedAt: 0,
        status: "idle",
        message: null,
    }
    writeVizState({ ...current, ...partial, updatedAt: Date.now() })
}

/** Overwrite the whole state file with a fresh idle state.  Must be called at
 *  daemon start so a stale `listening:true` left behind by a hard-killed
 *  previous session cannot make the viz show an empty overlay before
 *  dictation actually begins. */
export function resetVizState(pill: boolean, lang: string): void {
    writeVizState({
        listening: false,
        lang,
        error: null,
        loading: false,
        pill,
        dictationText: "",
        pillDirty: false,
        updatedAt: Date.now(),
        status: "idle",
        message: null,
    })
}

export function readVizState(): VizState | null {
    try {
        const path = statePath()
        const raw = readFileSync(path, "utf8")
        return JSON.parse(raw) as VizState
    } catch {
        return null
    }
}
