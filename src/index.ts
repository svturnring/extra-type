import { spawn } from "node:child_process"
import Daemon from "./daemon.js"
import { loadConfig, persistConfig } from "./config.js"
import { initLogger, log } from "./logger.js"
import { showHelp } from "./cli.js"
import { detectLanguage } from "./layoutProviders/index.js"
import { WSA_LANGUAGES } from "./constants.js"
import { isValidLanguage } from "./language.js"

process.title = "extra-type"
process.argv[0] = "extra-type"

const arg = process.argv[2]
const arg2 = process.argv[3]

if (arg === "help" || arg === "-h" || arg === "--help") {
    showHelp()
    process.exit(0)
}

async function main() {
    initLogger()

    const config = await loadConfig()
    const base = `http://127.0.0.1:${config.port}`

    const jsonGet = async (path: string, timeout = 1000) => {
        try {
            const res = await fetch(`${base}${path}`, { signal: AbortSignal.timeout(timeout) })
            return res.ok ? res : null
        } catch {
            return null
        }
    }

    const isDaemonUp = async () => (await jsonGet("/health", 1000)) !== null

    async function waitForDaemon(attempts = 40): Promise<boolean> {
        for (let i = 0; i < attempts; i++) {
            if (await isDaemonUp()) return true
            await new Promise((r) => setTimeout(r, 150))
        }
        return false
    }

    const spawnDaemonBackground = () => {
        const child = spawn(process.execPath, ["start"], {
            detached: true,
            stdio: "ignore",
            env: process.env,
        })
        child.unref()
    }

    async function ensureDaemon(): Promise<boolean> {
        if (await isDaemonUp()) return true
        spawnDaemonBackground()
        const up = await waitForDaemon()
        if (!up) {
            log("CLI", "daemon did not come up in time")
        }
        return up
    }

    async function doToggle() {
        const up = await ensureDaemon()
        if (!up) return
        const lang = config.autodetect_lang ? detectLanguage() : null
        const q = lang ? `?lang=${lang}` : ""
        for (let i = 0; i < 40; i++) {
            const res = await jsonGet(`/toggle${q}`, 15000)
            if (res) {
                const body = await res.text()
                console.log(body || "ok")
                return
            }
            await new Promise((r) => setTimeout(r, 300))
        }
        console.error("daemon did not become ready in time")
    }

    async function doStatus() {
        if (!(await isDaemonUp())) {
            console.log("extra-type daemon is not running")
            return
        }
        const res = await jsonGet("/status", 1500)
        if (!res) {
            console.log("could not read daemon status")
            return
        }
        const body = (await res.json()) as { listening?: boolean; lang?: string }
        const state = body.listening ? "listening" : "idle"
        console.log(`extra-type: ${state} (lang=${body.lang ?? "unknown"})`)
    }

    async function doStop() {
        if (!(await isDaemonUp())) {
            console.log("extra-type daemon is not running")
            return
        }
        await jsonGet("/exit", 1500)
        console.log("daemon stopped")
    }

    async function doSetLang(lang: string) {
        if (!isValidLanguage(lang)) {
            console.error(`Invalid language: ${lang}`)
            console.error(`Supported: ${Object.values(WSA_LANGUAGES).join(", ")}`)
            process.exit(1)
        }
        await persistConfig({ ...config, lang })
        console.log(`Language set to ${lang}`)
        if (await isDaemonUp()) {
            await jsonGet(`/setlang?lang=${lang}`, 2000)
        }
    }

    async function doAutodetect(onOff: string) {
        const enabled = onOff === "on" || onOff === "true" || onOff === "1"
        const disabled = onOff === "off" || onOff === "false" || onOff === "0"
        if (!enabled && !disabled) {
            console.error("Usage: extra-type autodetect on|off")
            process.exit(1)
        }
        await persistConfig({ ...config, autodetect_lang: enabled })
        const up = await isDaemonUp()
        if (up) {
            await fetch(`${base}/config`, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ autodetect_lang: enabled }),
            })
        }
        console.log(`Auto-detect layout: ${enabled ? "enabled" : "disabled"}`)
    }

    async function doOpenSettings() {
        const up = await ensureDaemon()
        if (!up) return
        await jsonGet("/open-settings", 3000)
    }

    const startForeground = async () => {
        const daemon = new Daemon(config)
        const destroy = async () => {
            await daemon.destroy()
            process.exit(0)
        }
        process.on("SIGTERM", destroy)
        process.on("SIGINT", destroy)
        await daemon.start()
    }

    if (arg === "update") {
        const { runUpdate } = await import("./updater.js")
        await runUpdate()
        return
    }

    if (arg === "toggle") {
        await doToggle()
        return
    }

    if (arg === "status") {
        await doStatus()
        return
    }

    if (arg === "stop") {
        await doStop()
        return
    }

    if (arg === "lang") {
        if (!arg2) {
            console.log(`Current language: ${config.lang}`)
            console.log(`Supported: ${Object.values(WSA_LANGUAGES).join(", ")}`)
            return
        }
        await doSetLang(arg2)
        return
    }

    if (arg === "autodetect") {
        if (!arg2) {
            console.log(`Auto-detect layout: ${config.autodetect_lang ? "enabled" : "disabled"}`)
            return
        }
        await doAutodetect(arg2)
        return
    }

    if (arg === "settings") {
        await doOpenSettings()
        return
    }

    if (arg !== undefined && arg !== "start") {
        showHelp()
        process.exit(0)
    }

    await startForeground()
}

main().catch((e) => {
    log("SYSTEM", "fatal:", e)
    process.exit(1)
})
