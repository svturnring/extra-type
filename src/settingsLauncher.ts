import { spawn, type ChildProcess } from "node:child_process"
import { existsSync } from "node:fs"
import { dirname, join } from "node:path"
import { log } from "./logger.js"

let child: ChildProcess | null = null

function resolveSettingsPath(): string | null {
    const candidates: string[] = ["/usr/local/bin/extra-type-settings"]
    try {
        candidates.push(join(dirname(process.execPath), "extra-type-settings"))
    } catch {
        /* execPath may be a stub; ignore */
    }
    for (const p of candidates) {
        if (p && existsSync(p)) return p
    }
    return null
}

/** Open the settings window. The window itself is single-instance (GtkApplication
 * raises the existing one), so spawning again is harmless — but we still avoid
 * stacking child processes while one is alive. */
export function openSettingsWindow(port: number = 3232): void {
    if (child) return
    const path = resolveSettingsPath()
    if (!path) {
        log("SETTINGS", "extra-type-settings binary not found")
        return
    }
    log("SETTINGS", `spawning ${path}`)
    child = spawn(path, [String(port)], {
        detached: true,
        stdio: "ignore",
    })
    child.unref()
    child.on("exit", () => {
        child = null
    })
    child.on("error", (e) => {
        log("SETTINGS", `settings spawn error: ${e.message}`)
        child = null
    })
}

/** Close the settings window when the daemon exits. */
export function closeSettingsWindow(): void {
    if (child) {
        try {
            child.kill("SIGTERM")
        } catch {
            /* ignore */
        }
        child = null
    }
}