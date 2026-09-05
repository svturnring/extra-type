import { spawn, type ChildProcess } from "node:child_process"
import { existsSync } from "node:fs"
import { dirname, join } from "node:path"
import { homedir } from "node:os"
import { log } from "./logger.js"

let child: ChildProcess | null = null

function resolveHotkeyPath(): string | null {
    const candidates: string[] = []
    candidates.push("/usr/local/bin/extra-type-hotkey")
    try {
        candidates.push(join(dirname(process.execPath), "extra-type-hotkey"))
    } catch {
        /* execPath may be a stub; ignore */
    }
    for (const p of candidates) {
        if (p && existsSync(p)) return p
    }
    return null
}

export function startHotkeyDaemon(port: number, combo: string): void {
    if (child) return
    const path = resolveHotkeyPath()
    if (!path) {
        log("HOTKEY", "extra-type-hotkey binary not found; global hotkey disabled")
        return
    }
    const logPath = `${homedir()}/.local/state/extra-type/hotkey.log`
    log("HOTKEY", `spawning ${path} --port ${port} --combo ${combo}`)
    child = spawn(path, ["--port", String(port), "--combo", combo, "--log-file", logPath], {
        detached: true,
        stdio: "ignore",
    })
    child.unref()
    child.on("exit", (code) => {
        log("HOTKEY", `helper exited (code ${code})`)
        child = null
    })
    child.on("error", (e) => {
        log("HOTKEY", `helper spawn error: ${e.message}`)
        child = null
    })
}

export function stopHotkeyDaemon(): void {
    if (child) {
        try {
            child.kill("SIGTERM")
        } catch {
            /* ignore */
        }
        child = null
    }
}

export function hotkeyRunning(): boolean {
    return child !== null
}