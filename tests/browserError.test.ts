import { describe, expect, test, beforeEach, afterEach } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import Daemon from "../src/daemon.js"
import TypingController from "../src/typingController.js"
import { readVizState } from "../src/vizState.js"
import type { DotoolSink } from "../src/dotoolSink.js"
import type { ExtraTypeConfig } from "../src/types.js"

const STUB_SINK: DotoolSink = {
    write() {},
    writable: false,
    kill() {},
}

const BASE_CONFIG: ExtraTypeConfig = {
    port: 0,
    lang: "en-US",
    browser_type: "chrome",
    browser_path: "",
    stream: true,
    timeout: 0,
    sound: false,
    text: false,
    punctuation: true,
    autodetect_lang: false,
    hotkey: null,
    paste: "ctrl+v",
    insert_method: "type",
    visualizer: { enabled: true, path: "" },
}

describe("Daemon.handleBrowserRecError", () => {
    let stateDir: string

    beforeEach(() => {
        stateDir = mkdtempSync(join(tmpdir(), "vt-err-"))
        process.env.XDG_STATE_HOME = stateDir
        const configDir = mkdtempSync(join(tmpdir(), "vt-errcfg-"))
        process.env.XDG_CONFIG_HOME = configDir
    })

    afterEach(() => {
        rmSync(stateDir, { recursive: true, force: true })
        delete process.env.XDG_STATE_HOME
        delete process.env.XDG_CONFIG_HOME
    })

    test("while listening sets status error, listening false, and a message", async () => {
        const daemon = new Daemon(BASE_CONFIG, new TypingController(STUB_SINK)) as unknown as {
            handleBrowserRecError(p: { code: string; message: string }): Promise<void>
            isWSAListening: boolean
            notifier: { notifyError(m: string): Promise<void> }
            tray: { listening: boolean }
        }
        daemon.isWSAListening = true
        let notified = ""
        daemon.notifier.notifyError = async (m: string) => {
            notified = m
        }
        daemon.tray.listening = true

        await daemon.handleBrowserRecError({ code: "not-allowed", message: "mic denied" })

        const viz = readVizState()
        expect(viz?.status).toBe("error")
        expect(viz?.message).toContain("mic denied")
        expect(viz?.listening).toBe(false)
        expect(daemon.isWSAListening).toBe(false)
        expect(daemon.tray.listening).toBe(false)
        expect(notified).toContain("mic denied")
    })

    test("when not listening still records error status", async () => {
        const daemon = new Daemon(BASE_CONFIG, new TypingController(STUB_SINK)) as unknown as {
            handleBrowserRecError(p: { code: string; message: string }): Promise<void>
            isWSAListening: boolean
        }
        daemon.isWSAListening = false

        await daemon.handleBrowserRecError({ code: "audio-capture", message: "no device" })

        const viz = readVizState()
        expect(viz?.status).toBe("error")
        expect(viz?.message).toContain("no device")
    })
})
