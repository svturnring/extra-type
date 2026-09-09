import { Browser, Page } from "puppeteer-core"
import * as browser from "./browser.js"
import TypingController from "./typingController.js"
import { log } from "./logger.js"
import { runPreflight } from "./preflight.js"
import { persistConfig } from "./config.js"
import express, { type Express, type Request, type Response, type NextFunction } from "express"
import Notifier from "./notifier.js"
import { launchBrowser } from "./browserLauncher.js"
import { isValidLanguage, readLanguageQuery } from "./language.js"
import { createTranscriptTransformerSession } from "./transcriptTransformers/index.js"
import { createNoopTransformerSession } from "./transcriptTransformers/noop.js"
import type { TranscriptTransformerSession } from "./transcriptTransformers/types.js"
import { createLayoutWatcher, type LayoutWatcher } from "./layoutWatcher.js"
import { Tray } from "./tray.js"
import { updateVizState } from "./vizState.js"
import { startVisualizer, stopVisualizer } from "./visualizer.js"
import { startHotkeyDaemon, stopHotkeyDaemon } from "./hotkey.js"
import { openSettingsWindow, closeSettingsWindow } from "./settingsLauncher.js"
import SpeechPipeline from "./speechPipeline.js"
import { shouldAcceptSpeechEvent } from "./speechEventGate.js"
import type { SpeechEvent, ExtraTypeConfig, VizStatus } from "./types.js"

export default class Daemon {
    private readonly config: ExtraTypeConfig
    private transcriptTransformer: TranscriptTransformerSession = createNoopTransformerSession()
    private typingController: TypingController = new TypingController(undefined, "ctrl+v", "type")
    private speechPipeline: SpeechPipeline = new SpeechPipeline(this.transcriptTransformer, this.typingController)
    private browser: Browser | null = null
    private page: Page | null = null
    private isWSAListening: boolean = false
    private lastToggleAt = 0
    private stallFastRetried = false
    public readonly app: Express

    private notifier: Notifier
    private stopCooldown: boolean = false
    private silenceTimer: NodeJS.Timeout | null = null
    private watchdogTimer: NodeJS.Timeout | null = null
    private recovering: boolean = false
    private startConfirmTimer: NodeJS.Timeout | null = null
    private punctuationEnabled: boolean
    private currentLang: string
    private layoutWatcher: LayoutWatcher = createLayoutWatcher((lang) => {
        if (lang !== this.currentLang) {
            void this.applyLanguage(lang)
        }
    })
    private tray: Tray = new Tray({
        onActivate: () => {
            void this.toggleFromTray()
        },
        onSecondaryActivate: () => {
            void this.toggleFromTray()
        },
        onSettings: () => {
            void this.openSettings()
        },
        onQuit: () => {
            void (async () => {
                await this.destroy()
                process.exit(0)
            })()
        },
    })

    constructor(config: ExtraTypeConfig, typingController?: TypingController) {
        this.config = config
        if (typingController) {
            this.typingController = typingController
            this.speechPipeline = new SpeechPipeline(this.transcriptTransformer, this.typingController)
        } else {
            this.typingController = new TypingController(undefined, config.paste, config.insert_method)
            this.speechPipeline = new SpeechPipeline(this.transcriptTransformer, this.typingController)
        }
        this.typingController.setPillListener((text) => {
            updateVizState({ pill: true, dictationText: text, pillDirty: false })
        })
        this.punctuationEnabled = config.punctuation
        this.currentLang = config.insert_method === "dotool" ? "en-US" : config.lang
        this.app = express()
        this.app.use(express.json())
        this.setupRoutes()
        this.notifier = new Notifier({
            textNotifsEnabled: true,
            soundsNotifsEnabled: config.sound,
        })
    }

    private setupRoutes() {
        this.app.get("/health", (req, res) => {
            res.json({ status: "ok" })
        })

        this.app.get("/status", (req, res) => {
            res.json({ listening: this.isWSAListening, lang: this.currentLang })
        })

        this.app.get("/setlang", this.browserHealthMiddleware.bind(this), async (req, res) => {
            const lang = this.resolveAndValidateLanguage(req, res)
            if (lang === null) return
            await this.applyLanguage(lang, res)
        })

        this.app.get("/start", this.browserHealthMiddleware.bind(this), async (req, res) => {
            const lang = this.resolveAndValidateLanguage(req, res)
            if (lang === null) return
            await this.startTranscription(lang, res)
        })

        this.app.get("/stop", this.browserHealthMiddleware.bind(this), async (req, res) => {
            await this.stopTranscription("intentional", res)
        })

        this.app.get("/toggle", this.browserHealthMiddleware.bind(this), async (req, res) => {
            const now = Date.now()
            if (now - this.lastToggleAt < 800) {
                res.json({ listening: this.isWSAListening })
                return
            }
            this.lastToggleAt = now
            if (this.isWSAListening) {
                await this.stopTranscription("intentional", res)
            } else {
                const lang = this.resolveAndValidateLanguage(req, res)
                if (lang === null) return
                await this.startTranscription(lang, res)
            }
        })

        this.app.get("/exit", async (req, res) => {
            await this.notifier.notifyDaemonStop()
            res.send("Stopped daemon")
            await this.destroy()
            process.exit(0)
        })

        this.app.get("/config", (req, res) => {
            res.json({
                port: this.config.port,
                lang: this.config.lang,
                browser_type: this.config.browser_type,
                browser_path: this.config.browser_path,
                timeout: this.config.timeout,
                sound: this.config.sound,
                text: this.config.text,
                punctuation: this.punctuationEnabled,
                autodetect_lang: this.config.autodetect_lang,
                hotkey: this.config.hotkey,
                paste: this.config.paste,
                insert_method: this.config.insert_method,
                stream: this.config.stream,
                visualizer: this.config.visualizer,
            })
        })

this.app.post("/config", async (req, res) => {
             try {
                 const body = req.body as Record<string, unknown> | undefined
if (!body || typeof body !== "object") {
                    log("DAEMON", `POST /config bad/empty body, headers=${JSON.stringify(req.headers)}`)
                    res.status(400).json({ error: "JSON body required" })
                    return
                }
                log("DAEMON", `POST /config received: ${JSON.stringify(body)}`)
                if (typeof body.lang === "string") {
                    this.config.lang = body.lang
                    this.currentLang = body.lang
                }
                if (typeof body.autodetect_lang === "boolean") {
                    this.config.autodetect_lang = body.autodetect_lang
                    if (this.config.autodetect_lang) {
                        this.layoutWatcher.start()
                    } else {
                        this.layoutWatcher.stop()
                    }
                }
                if (typeof body.sound === "boolean") {
                    this.config.sound = body.sound
                    this.notifier.setSoundEnabled(body.sound)
                }
                if (typeof body.text === "boolean") {
                    this.config.text = body.text
                }
                if (typeof body.punctuation === "boolean") {
                    this.punctuationEnabled = body.punctuation
                }
                if (typeof body.timeout === "number" && body.timeout >= 0) {
                    this.config.timeout = body.timeout
                }
                if ("hotkey" in body) {
                    const hk = body.hotkey
                    if (hk === null || (typeof hk === "string" && hk.length > 0)) {
                        this.config.hotkey = hk
                        this.syncHotkey()
                    } else {
                        log("DAEMON", "POST /config: invalid hotkey, ignored")
                    }
                }
                if ("paste" in body) {
                    if (typeof body.paste === "string" && body.paste.length > 0) {
                        this.config.paste = body.paste
                        this.typingController.setPasteCombo(body.paste)
                    } else {
                        log("DAEMON", "POST /config: invalid paste combo, ignored")
                    }
                }
                if ("insert_method" in body) {
                    if (body.insert_method === "type" || body.insert_method === "paste" || body.insert_method === "pill" || body.insert_method === "dotool") {
                        this.config.insert_method = body.insert_method
                        this.typingController.setInsertMethod(body.insert_method)
                        if (body.insert_method !== "pill") {
                            updateVizState({ pill: false, dictationText: "" })
                        } else {
                            updateVizState({ pill: true, dictationText: "" })
                        }
                        if (body.insert_method === "dotool") {
                            /* dotool is English-only (resolved against the US
                             * layout); force the language and stop auto-detect
                             * so recognition runs with en-US. */
                            if (this.config.autodetect_lang) {
                                this.config.autodetect_lang = false
                                this.layoutWatcher.stop()
                            }
                            if (this.currentLang !== "en-US") {
                                this.config.lang = "en-US"
                                await this.applyLanguage("en-US")
                            }
                        }
                    } else {
                        log("DAEMON", "POST /config: invalid insert_method, ignored")
                    }
                }
                if (typeof body.stream === "boolean") {
                    this.config.stream = body.stream
                    /* interimResults is fixed in the WSA object at init time, so a
                     * stream toggle must recreate the recognizer — otherwise it
                     * never actually turns live typing off (or on). */
                    this.transcriptTransformer = createTranscriptTransformerSession(
                        this.currentLang,
                        this.config.stream,
                        () => this.punctuationEnabled,
                    )
                    this.speechPipeline = new SpeechPipeline(this.transcriptTransformer, this.typingController)
                    if (this.isWSAListening) {
                        await this.recoverBrowserWhileListening()
                    } else if (this.page) {
                        try {
                            await this.page.evaluate(browser.initWSA, this.config.stream, this.currentLang)
                        } catch (e) {
                            log("DAEMON", `stream reinit failed: ${e}`)
                        }
                    }
                }
                await persistConfig(this.config)
                res.json({ ok: true })
            } catch (e) {
                log("DAEMON", `POST /config failed: ${e}`)
                res.status(500).json({ error: String(e) })
            }
        })

        this.app.post("/pill/clear", async (req, res) => {
            this.typingController.clearPill()
            updateVizState({ pill: this.config.insert_method === "pill", dictationText: "", pillDirty: false })
            res.json({ ok: true })
        })

        this.app.post("/pill/update", async (req, res) => {
            const text = (req.body as Record<string, unknown> | undefined)?.text
            if (typeof text !== "string") {
                res.status(400).json({ error: "text required" })
                return
            }
            this.typingController.setPillText(text)
            updateVizState({ pill: true, dictationText: text, pillDirty: true })
            res.json({ ok: true })
        })

        this.app.get("/open-settings", async (req, res) => {
            res.send("ok")
            openSettingsWindow(this.config.port)
        })

        this.app.get("/togglePunctuation", async (req, res) => {
            const raw = (req.query as Record<string, unknown>).enabled
            if (raw !== undefined) {
                if (raw === "true") {
                    this.punctuationEnabled = true
                } else if (raw === "false") {
                    this.punctuationEnabled = false
                } else {
                    res.status(400).json({ error: "enabled must be 'true' or 'false'" })
                    return
                }
            } else {
                this.punctuationEnabled = !this.punctuationEnabled
            }
            try {
                await persistConfig({ ...this.config, punctuation: this.punctuationEnabled })
            } catch (e) {
                log("DAEMON", `Failed to persist punctuation toggle: ${e}`)
            }
            log("DAEMON", `punctuation ${this.punctuationEnabled ? "enabled" : "disabled"}`)
            res.json({ punctuation: this.punctuationEnabled })
        })
    }

    private resolveAndValidateLanguage(req: Request, res: Response): string | null {
        const requested = readLanguageQuery(req.query as Record<string, unknown>)
        if (requested === undefined) {
            return this.config.lang
        }
        if (!isValidLanguage(requested)) {
            log("DAEMON", `Invalid language param '${requested}'`)
            void this.notifier.notifyError(`Invalid language: ${requested}`)
            res.status(400).send(`Invalid language: ${requested}`)
            return null
        }
        return requested
    }

    private async startTranscription(lang: string, res?: Response) {
        if (this.stopCooldown) {
            res?.status(429).send("Cooldown active - wait before starting")
            return
        }
        if (this.isWSAListening) {
            res?.send("Listener already active")
            return
        }

        log("DAEMON", `Starting transcription in '${lang}'...`)
        this.currentLang = lang
        this.transcriptTransformer = createTranscriptTransformerSession(
            lang,
            this.config.stream,
            () => this.punctuationEnabled,
        )
        this.transcriptTransformer.reset()
        this.speechPipeline = new SpeechPipeline(this.transcriptTransformer, this.typingController)
        this.isWSAListening = true
        this.stallFastRetried = false
        this.tray.listening = true
        this.typingController.hasStopped = false
        this.notifier.notifyMicStart()
        updateVizState({ listening: true, lang, error: null, loading: true, status: "starting" })

        if (this.config.stream && this.config.timeout > 0) {
            this.resetSilenceTimer()
        }

        this.clearStartConfirmTimer()
        this.startConfirmTimer = setTimeout(() => {
            this.startConfirmTimer = null
            if (!this.isWSAListening) return
            log("DAEMON", "Start confirmation timeout — recognition never started")
            this.isWSAListening = false
            this.tray.listening = false
            updateVizState({ error: "Recognition did not start", listening: false, loading: false, status: "error", message: "Recognition did not start" })
            void this.notifier.notifyError("Recognition did not start")
        }, 8000)

        await this.page!.evaluate(browser.setLangAndStart, lang)
        res?.send("Listening")
    }

    private async reinitBrowser() {
        if (this.browser) {
            try {
                await this.browser.close()
            } catch {}
        }
        this.browser = null
        this.page = null
        this.isWSAListening = false
        updateVizState({ loading: true, error: null, status: "starting" })
        try {
            await this.initBrowser()
            updateVizState({ listening: false, loading: false, error: null, status: "idle" })
        } catch (e) {
            updateVizState({ loading: false, error: "Browser restart failed", status: "error", message: "Browser restart failed" })
            throw e
        }
    }

    private async stopTranscription(reason: "intentional" | "offline" | "silence" | "error", res?: Response) {
        if (this.stopCooldown) {
            log("DAEMON", `Stop request ignored - still in cooldown period (reason: ${reason})`)
            res?.status(429).send("Cooldown active")
            return
        }
        if (!this.isWSAListening) {
            log("DAEMON", "No active listener.")
            res?.send("No active listener")
            return
        }
        this.clearSilenceTimer()

        log("DAEMON", `Stopping transcription... Reason: ${reason}`)
        this.isWSAListening = false
        this.clearStartConfirmTimer()
        this.tray.listening = false
        this.transcriptTransformer.reset()
        this.typingController.hasStopped = true
        if (this.config.insert_method === "pill") {
            const text = this.typingController.getPillText()
            if (text) {
                try {
                    const { spawnSync } = await import("node:child_process")
                    spawnSync("wl-copy", ["--type", "text/plain;charset=utf-8", "--", text])
                    log("DAEMON", "pill transcript copied to clipboard on stop")
                } catch (e) {
                    log("DAEMON", `failed to copy pill text to clipboard: ${e}`)
                }
            }
            updateVizState({ dictationText: "", pillDirty: false })
        }
        this.typingController.reset()
        updateVizState({ listening: false, error: null, loading: false, status: "idle" })

        if (reason === "intentional") {
            this.notifier.notifyMicStopIntentional()
        } else if (reason === "silence") {
            this.notifier.notifyMicStopSilence()
        } else if (reason === "offline") {
            this.notifier.notifyOffline()
        } else if (reason === "error") {
            this.notifier.notifyError("Recognition stopped due to error")
        }

        this.stopCooldown = true
        setTimeout(() => {
            this.stopCooldown = false
        }, 100)

        await this.page!.evaluate(browser.setStopRequested, true)
        await this.page!.evaluate(browser.stopRecognition)
        res?.send("Stopped")
    }

    private isBrowserReady(): boolean {
        return this.page !== null && this.browser !== null
    }

    private async browserHealthMiddleware(req: Request, res: Response, next: NextFunction) {
        if (!this.isBrowserReady()) {
            log("DAEMON", "Browser not ready")
            res.status(503).send("Browser not ready")
            return
        }

        try {
            await this.page!.evaluate(browser.healthCheck)
            next()
        } catch (e) {
            log("DAEMON", `Browser health check failed: ${e} - reinitializing...`)
            const wasListening = this.isWSAListening
            if (wasListening) {
                const ok = await this.recoverBrowserWhileListening()
                if (!ok) {
                    res.status(503).send("Browser reinitialization failed")
                    return
                }
                next()
            } else {
                try {
                    await this.reinitBrowser()
                    next()
                } catch (e2) {
                    log("DAEMON", `Browser reinitialization failed: ${e2}`)
                    updateVizState({ error: "Browser reinitialization failed", loading: false, status: "error", message: "Browser reinitialization failed" })
                    res.status(503).send("Browser reinitialization failed")
                }
            }
        }
    }

    /** Periodic browser liveness check so the pill never lies while listening. */
    private async watchdogTick() {
        if (!this.isWSAListening || this.recovering) return
        if (!this.isBrowserReady()) {
            log("DAEMON", "Watchdog: browser not ready while listening")
            await this.runRecovery()
            return
        }
        let health: { state: string; lastError: string | null; restartCount: number }
        try {
            health = await this.page!.evaluate(browser.healthCheck)
        } catch (e) {
            log("DAEMON", `Watchdog: page health check failed: ${e}`)
            await this.runRecovery()
            return
        }
        if (health.state === "no-recognition") {
            log("DAEMON", "Watchdog: recognition missing while listening")
            await this.runRecovery()
            return
        }
        if (health.state === "error") {
            log("DAEMON", `Watchdog: recognition error — ${health.lastError}`)
            this.setStatus("error", health.lastError ?? "Unknown error")
            this.tray.listening = false
            this.isWSAListening = false
            updateVizState({ listening: false })
            await this.notifier.notifyError(`Recognition error: ${health.lastError}`)
            return
        }
        if (health.state === "stalled") {
            if (!this.stallFastRetried) {
                this.stallFastRetried = true
                log("DAEMON", "Watchdog: recognition hung — fast in-place restart")
                this.setStatus("recovering", "Recovering…")
                updateVizState({ loading: true, error: null })
                try {
                    await this.page!.evaluate(browser.setLangAndStart, this.currentLang)
                } catch (e) {
                    log("DAEMON", `Watchdog: fast restart failed: ${e}`)
                    this.stallFastRetried = false
                    await this.runRecovery()
                }
                return
            }
            this.stallFastRetried = false
            log("DAEMON", "Watchdog: recognition still hung — full browser restart")
            await this.runRecovery()
            return
        }
        if (health.state === "listening") {
            this.stallFastRetried = false
            return
        }
        if (health.state === "idle") {
            if (health.lastError) {
                log("DAEMON", `Watchdog: idle with error — ${health.lastError}`)
                this.setStatus("error", health.lastError)
                this.isWSAListening = false
                this.tray.listening = false
                updateVizState({ listening: false })
                await this.notifier.notifyError(`Recognition error: ${health.lastError}`)
                return
            }
            await new Promise((r) => setTimeout(r, 1500))
            if (!this.isWSAListening || this.recovering) return
            try {
                health = await this.page!.evaluate(browser.healthCheck)
            } catch (e) {
                log("DAEMON", `Watchdog: re-check failed: ${e}`)
                await this.runRecovery()
                return
            }
            if (health.state === "listening") return
            log("DAEMON", "Watchdog: recognition stuck idle while listening — restarting")
            await this.runRecovery()
        }
    }

    private async runRecovery() {
        this.recovering = true
        try {
            await this.recoverBrowserWhileListening()
        } finally {
            this.recovering = false
        }
    }

    /** Restart the browser page and resume recognition, feeding the pill states. */
    private async recoverBrowserWhileListening(): Promise<boolean> {
        log("DAEMON", "Recovering browser while listening...")
        /* the WSA session is being rebuilt; the on-screen interim can no longer
         * be diffed against new partials — fold it away so the first partial
         * after the restart doesn't wipe the already-dictated text */
        this.typingController.recoverRestart()
        this.setStatus("recovering", "Recovering…")
        updateVizState({ loading: true, error: null })
        try {
            await this.reinitBrowser()
        } catch (e) {
            log("DAEMON", `Browser reinit failed while listening: ${e}`)
            this.isWSAListening = false
            this.tray.listening = false
            updateVizState({ error: "Recognition unavailable", listening: false, loading: false, status: "error", message: "Recognition unavailable" })
            await this.notifier.notifyOffline()
            return false
        }
        this.isWSAListening = true
        this.typingController.hasStopped = false
        this.clearStartConfirmTimer()
        this.startConfirmTimer = setTimeout(() => {
            this.startConfirmTimer = null
            if (!this.isWSAListening) return
            log("DAEMON", "Recovery confirmation timeout — recognition never restarted")
            this.isWSAListening = false
            this.tray.listening = false
            updateVizState({ error: "Recognition did not restart", listening: false, loading: false, status: "error", message: "Recognition did not restart" })
            void this.notifier.notifyError("Recognition did not restart")
        }, 8000)
        try {
            await this.page!.evaluate(browser.setLangAndStart, this.currentLang)
            this.stallFastRetried = false
            log("DAEMON", "Recognition resumed after restart")
        } catch (e) {
            log("DAEMON", `Failed to resume recognition: ${e}`)
            this.isWSAListening = false
            this.tray.listening = false
            updateVizState({ error: "Recognition failed", listening: false, loading: false, status: "error", message: "Recognition failed" })
            await this.notifier.notifyOffline()
            return false
        }
        return true
    }

    private async initBrowser() {
        this.browser = await launchBrowser(this.config)
        this.page = await this.browser.newPage()
        this.page.on("console", (msg) => log("BROWSER", msg.text()))

        await this.page.goto("data:text/html,<html><body><h1>Extra Type</h1></body></html>")
        await this.page.exposeFunction("onSpeechEvent", this.handleSpeechEvent.bind(this))
        await this.page.exposeFunction("onBrowserRecStop", this.handleBrowserRecStop.bind(this))
        await this.page.exposeFunction("onBrowserRecError", this.handleBrowserRecError.bind(this))
        await this.page.exposeFunction("onBrowserRecRestart", this.handleBrowserRecRestart.bind(this))
        await this.page.exposeFunction("onBrowserRecStart", this.handleBrowserRecStart.bind(this))
        await this.page.evaluate(browser.initWSA, this.config.stream, this.config.lang)
    }

    private handleSpeechEvent(event: SpeechEvent) {
        if (!shouldAcceptSpeechEvent(this.isWSAListening, this.typingController.hasStopped)) return

        this.speechPipeline.onEvent(event)
        if (event.kind === "text" && this.config.stream && this.config.timeout > 0) {
            this.resetSilenceTimer()
        }
    }

    private resetSilenceTimer() {
        this.clearSilenceTimer()
        if (this.config.timeout > 0) {
            this.silenceTimer = setTimeout(() => {
                if (!this.isWSAListening) return
                void this.stopTranscription("silence").catch((e) => {
                    log("DAEMON", `Silence timer stop failed: ${e}`)
                })
            }, this.config.timeout * 1000)
        }
    }

    private clearSilenceTimer() {
        if (this.silenceTimer) {
            clearTimeout(this.silenceTimer)
            this.silenceTimer = null
        }
    }
    private clearStartConfirmTimer() {
        if (this.startConfirmTimer) {
            clearTimeout(this.startConfirmTimer)
            this.startConfirmTimer = null
        }
    }
    private async handleBrowserRecStop(payload: { reason: "silence" | "offline" | "error" | undefined }) {
        if (!this.isWSAListening) return
        try {
            await this.stopTranscription(payload.reason === "error" ? "error" : (payload.reason ?? "silence"))
        } catch (e) {
            log("DAEMON", `Browser rec stop handling failed: ${e}`)
        }
    }

    private setStatus(status: VizStatus, message?: string) {
        updateVizState({ status, message: message ?? null })
    }

    private handleBrowserRecRestart() {
        log("DAEMON", "Browser recognition session ended - resetting live-text diff")
        this.typingController.recoverRestart()
    }

    private handleBrowserRecStart(payload: Record<string, unknown>) {
        void payload
        log("DAEMON", "Browser recognition started - confirmed by WSA")
        if (this.isWSAListening) {
            this.clearStartConfirmTimer()
            this.setStatus("listening")
            updateVizState({ listening: true, error: null, loading: false })
        }
    }

    private async handleBrowserRecError(payload: { code: string; message: string }) {
        log("DAEMON", `Browser recognition error: ${payload.code} — ${payload.message}`)
        if (this.isWSAListening) {
            this.isWSAListening = false
            this.setStatus("error", `${payload.code}: ${payload.message}`)
            updateVizState({ listening: false })
            this.tray.listening = false
            await this.notifier.notifyError(`Recognition error: ${payload.message}`)
        } else {
            this.setStatus("error", payload.message)
        }
    }

    private async applyLanguage(lang: string, res?: Response) {
        this.currentLang = lang
        updateVizState({ lang })
        if (this.isWSAListening) {
            log("DAEMON", `Switching live language to '${lang}'...`)
            /* restarting WSA in place — same interim wipe hazard as a recover */
            this.typingController.recoverRestart()
            updateVizState({ loading: true, status: "recovering", message: "Switching language…" })
            await this.page!.evaluate(browser.setStopRequested, true)
            await this.page!.evaluate(browser.stopRecognition)
            this.clearSilenceTimer()
            this.transcriptTransformer.reset()
            this.typingController.reset()
            this.transcriptTransformer = createTranscriptTransformerSession(
                lang,
                this.config.stream,
                () => this.punctuationEnabled,
            )
            this.transcriptTransformer.reset()
            this.speechPipeline = new SpeechPipeline(this.transcriptTransformer, this.typingController)
            this.clearStartConfirmTimer()
            this.startConfirmTimer = setTimeout(() => {
                this.startConfirmTimer = null
                if (!this.isWSAListening) return
                log("DAEMON", "Language switch confirmation timeout")
                this.isWSAListening = false
                this.tray.listening = false
                updateVizState({ error: "Recognition did not restart after language switch", listening: false, loading: false, status: "error", message: "Recognition did not restart after language switch" })
                void this.notifier.notifyError("Recognition did not restart after language switch")
            }, 8000)
            await this.page!.evaluate(browser.setLangAndStart, lang)
            if (res) res.json({ listening: true, lang })
        } else {
            log("DAEMON", `Stored language for next start: '${lang}'`)
            try {
                await persistConfig({ ...this.config, lang })
            } catch (e) {
                log("DAEMON", `Failed to persist language: ${e}`)
            }
            if (res) res.json({ listening: false, lang })
        }
    }

    private async toggleFromTray() {
        if (this.isWSAListening) {
            await this.stopTranscription("intentional")
        } else {
            await this.startTranscription(this.currentLang)
        }
    }

    private async openSettings() {
        openSettingsWindow(this.config.port)
    }

    private syncHotkey() {
        stopHotkeyDaemon()
        if (this.config.hotkey) {
            startHotkeyDaemon(this.config.port, this.config.hotkey)
        }
    }

    public async start() {
        const result = await runPreflight({
            port: this.config.port,
            browser_path: this.config.browser_path,
        })
        if (!result.ok) {
            const { kind, message } = result.failure
            if (kind === "port-in-use") {
                await this.notifier.notifyAlreadyRunning()
                log("DAEMON", message)
                process.exit(0)
            }
            await this.notifier.notifyError(message)
            log("PREFLIGHT", message)
            log("DAEMON", `startup failure: ${message}`)
            await this.destroy()
            process.exit(1)
        }

        try {
            this.app.listen(this.config.port, "127.0.0.1", () => {
                log("DAEMON", `server started on port: ${this.config.port}`)
            })
            await this.initBrowser()
            if (this.config.autodetect_lang) {
                this.layoutWatcher.start()
            }
            await this.tray.start()
            if (this.config.visualizer.enabled) {
                startVisualizer(this.config.visualizer.path, this.config.port)
            }
            if (this.config.hotkey) {
                startHotkeyDaemon(this.config.port, this.config.hotkey)
            }
            this.watchdogTimer = setInterval(() => void this.watchdogTick(), 5000)
            this.notifier.notifyDaemonStart()
        } catch (e) {
            this.notifier.notifyError("Failed to initialize Extra Type daemon.")
            log("DAEMON", "Failed to initialize Extra Type daemon:", e)
            await this.destroy()
            process.exit(1)
        }
    }

    public async destroy() {
        log("DAEMON", "Shutting down daemon...")
        this.layoutWatcher.stop()
        this.tray.dispose()
        this.clearSilenceTimer()
        if (this.watchdogTimer) {
            clearInterval(this.watchdogTimer)
            this.watchdogTimer = null
        }
        this.transcriptTransformer.reset()
        this.notifier.destroy()
        this.typingController.destroy()
        stopVisualizer()
        stopHotkeyDaemon()
        closeSettingsWindow()

        await Promise.race([
            Promise.all([
                this.page?.close().catch(() => {}),
                this.browser?.close().catch(() => {}),
            ]),
            new Promise<void>((resolve) => setTimeout(resolve, 3000)),
        ])
        this.page = null
        this.browser = null
    }
}

