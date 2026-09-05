import { spawn, type ChildProcess } from "node:child_process"
import { existsSync } from "node:fs"
import { dirname, join } from "node:path"
import { log } from "./logger.js"

let child: ChildProcess | null = null

function resolveVizPath(configured: string): string | null {
    const candidates: string[] = []
    if (configured) candidates.push(configured)
    candidates.push("/usr/local/bin/extra-type-viz")
    try {
        candidates.push(join(dirname(process.execPath), "extra-type-viz"))
    } catch {
        /* execPath may be a stub; ignore */
    }
    for (const p of candidates) {
        if (p && existsSync(p)) return p
    }
    return null
}

export function startVisualizer(configuredPath: string, port: number = 3232): void {
    if (child) return
    const path = resolveVizPath(configuredPath)
    if (!path) {
        log("VIZ", "extra-type-viz binary not found; visualizer disabled")
        return
    }
    const env = { ...process.env }
    if (!env.SPA_PLUGIN_DIR) env.SPA_PLUGIN_DIR = "/usr/lib/spa-0.2"
    if (!env.PIPEWIRE_MODULE_DIR) env.PIPEWIRE_MODULE_DIR = "/usr/lib/pipewire-0.3"
    log("VIZ", `spawning ${path}`)
    child = spawn(path, ["--listen", "--port", String(port)], {
        detached: true,
        stdio: "ignore",
        env,
    })
    child.unref()
    child.on("exit", () => {
        child = null
    })
    child.on("error", (e) => {
        log("VIZ", `visualizer spawn error: ${e.message}`)
        child = null
    })
}

export function stopVisualizer(): void {
    if (child) {
        try {
            child.kill("SIGTERM")
        } catch {
            /* ignore */
        }
        child = null
    }
}
