import { homedir } from "node:os"
import { mkdirSync, writeFileSync, readFileSync } from "node:fs"
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
        writeFileSync(path, JSON.stringify(state), "utf8")
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

export function readVizState(): VizState | null {
    try {
        const path = statePath()
        const raw = readFileSync(path, "utf8")
        return JSON.parse(raw) as VizState
    } catch {
        return null
    }
}
