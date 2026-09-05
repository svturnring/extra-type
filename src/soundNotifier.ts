import { spawn } from "child_process"
import { delimiter, join, resolve } from "path"
import { existsSync, writeFileSync } from "fs"
import { tmpdir } from "os"
import { log } from "./logger.js"
import { START_SOUND_B64, STOP_SOUND_B64 } from "./soundsData.js"

const SYSTEM_EVENT_IDS: Record<string, string> = {
    notifyOffline: "dialog-error",
    notifyError: "dialog-error",
    notifyAlreadyRunning: "dialog-error",
}

const XDG_BASES: string[] = (() => {
    const bases: string[] = []
    const home = process.env.XDG_DATA_HOME || resolve(process.env.HOME || "/home", ".local/share")
    bases.push(join(home, "sounds"))
    const dirs = process.env.XDG_DATA_DIRS || "/usr/local/share/:/usr/share/"
    for (const d of dirs.split(":")) {
        const trimmed = d.trim()
        if (trimmed) bases.push(join(trimmed, "sounds"))
    }
    return bases
})()

const START_FILE = join(tmpdir(), "extra-type-start.mp3")
const STOP_FILE = join(tmpdir(), "extra-type-stop.mp3")

type Player = "canberra" | "paplay" | "pw-play" | "sox" | "ffplay" | "mpv" | null

function detectPlayer(): Player {
    if (isOnPath("canberra-gtk-play")) return "canberra"
    if (isOnPath("mpv")) return "mpv"
    if (isOnPath("ffplay")) return "ffplay"
    if (isOnPath("pw-play")) return "pw-play"
    if (isOnPath("paplay")) return "paplay"
    if (isOnPath("sox")) return "sox"
    return null
}

function isOnPath(cmd: string): boolean {
    const dirs = (process.env.PATH || "").split(delimiter)
    for (const dir of dirs) {
        if (existsSync(join(dir, cmd))) return true
    }
    return false
}

let PLAYER: Player | null = null

function getPlayer(): Player {
    if (PLAYER === null) PLAYER = detectPlayer()
    return PLAYER
}

const EXTENSIONS = ["disabled", "oga", "ogg", "wav"]

export function findSoundInBases(
    bases: string[],
    name: string,
    fileExists: (path: string) => boolean = existsSync,
): string | null {
    for (const base of bases) {
        const themed = join(base, "freedesktop", "stereo", name)
        for (const ext of EXTENSIONS) {
            const p = `${themed}.${ext}`
            if (ext === "disabled" && fileExists(p)) return null
            if (ext !== "disabled" && fileExists(p)) return p
        }
    }
    return null
}

export function resolveXdgSound(name: string): string | null {
    return findSoundInBases(XDG_BASES, name)
}

function ensureEmbeddedFile(path: string, b64: string, label: string): void {
    if (existsSync(path)) return
    try {
        writeFileSync(path, Buffer.from(b64, "base64"))
    } catch (err) {
        log("SOUND", `cannot write ${label} file:`, err)
    }
}

function ensureEmbeddedSounds(): void {
    ensureEmbeddedFile(START_FILE, START_SOUND_B64, "start sound")
    ensureEmbeddedFile(STOP_FILE, STOP_SOUND_B64, "stop sound")
}

/**
 * Sound notifications. Embedded start/stop sounds for recording start and stop,
 * system sounds (dialog-error) for offline and error events.
 */
export class SoundNotifier {
    private enabled: boolean

    constructor(enabled: boolean = true) {
        this.enabled = enabled
    }

    setEnabled(enabled: boolean) {
        this.enabled = enabled
    }

    private spawnNoThrow(cmd: string, args: string[]): Promise<void> {
        return new Promise((resolve) => {
            const proc = spawn(cmd, args)

            proc.on("error", (err) => {
                log("SOUND", `${cmd} error:`, err)
                resolve()
            })

            proc.on("close", () => {
                resolve()
            })
        })
    }

    /** Play an arbitrary audio file, preferring players with broad codec support (mpv/ffplay). */
    private async playFile(path: string): Promise<void> {
        if (!existsSync(path)) {
            log("SOUND", `sound file not found: ${path}`)
            return
        }
        if (isOnPath("mpv")) {
            return this.spawnNoThrow("mpv", ["--no-video", "--really-quiet", "--no-terminal", path])
        }
        if (isOnPath("ffplay")) {
            return this.spawnNoThrow("ffplay", ["-nodisp", "-autoexit", "-loglevel", "quiet", path])
        }
        if (isOnPath("pw-play")) return this.spawnNoThrow("pw-play", [path])
        if (isOnPath("canberra-gtk-play")) return this.spawnNoThrow("canberra-gtk-play", ["-f", path])
        return Promise.resolve()
    }

    private async notifySystem(eventId: string): Promise<void> {
        if (PLAYER === null) getPlayer()
        if (getPlayer() === "canberra") {
            return this.spawnNoThrow("canberra-gtk-play", [`--id=${eventId}`])
        }
        if (getPlayer() === "pw-play" || getPlayer() === "paplay") {
            const path = resolveXdgSound(eventId)
            if (!path) return Promise.resolve()
            const cmd = getPlayer() === "pw-play" ? "pw-play" : "paplay"
            return this.spawnNoThrow(cmd, [path])
        }
        return Promise.resolve()
    }

    async notifyStart() {
        if (!this.enabled) return
        ensureEmbeddedSounds()
        log("SOUND", `start sound: ${START_FILE}`)
        await this.playFile(START_FILE)
    }

    async notifyStop() {
        if (!this.enabled) return
        ensureEmbeddedSounds()
        log("SOUND", `stop sound: ${STOP_FILE}`)
        await this.playFile(STOP_FILE)
    }

    async notifyOffline() {
        if (!this.enabled) return
        await this.notifySystem(SYSTEM_EVENT_IDS["notifyOffline"])
    }

    async notifyError() {
        if (!this.enabled) return
        await this.notifySystem(SYSTEM_EVENT_IDS["notifyError"])
    }

    async notifyAlreadyRunning() {
        if (!this.enabled) return
        await this.notifySystem(SYSTEM_EVENT_IDS["notifyAlreadyRunning"])
    }
}