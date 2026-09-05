// Injected into Chrome via page.evaluate — no imports. Helpers used by evaluated
// functions must be nested inside them (Puppeteer only serializes the top-level export).
// Event mapper: keep browserRecognition.js in sync (tests/browserRecognition.sync.test.ts).

export function initWSA(stream, lang) {
    function recognitionResultsToEvents(stream, resultIndex, results) {
        let interimText = ""
        let finalizedText = ""
        let segmentFinalized = false

        for (let i = resultIndex; i < results.length; i++) {
            if (!results[i].isFinal) {
                interimText += results[i][0].transcript
            } else {
                finalizedText += results[i][0].transcript
                segmentFinalized = true
            }
        }

        const events = []

        if (stream) {
            if (segmentFinalized) {
                if (finalizedText) {
                    events.push({ kind: "text", text: finalizedText })
                }
                events.push({ kind: "segment-finalized" })
            }
            if (interimText) {
                events.push({ kind: "text", text: interimText })
            }
        } else if (segmentFinalized) {
            if (finalizedText) {
                events.push({ kind: "text", text: finalizedText })
            }
            events.push({ kind: "segment-finalized" })
        }

        return events
    }

    console.log("browser connected. initializing Web Speech API...")

    const SpeechRec = window.SpeechRecognition || window.webkitSpeechRecognition
    if (!SpeechRec) {
        console.error("FATAL: Web Speech API is not supported in this browser context.")
        throw new Error("FATAL: Web Speech API is not supported in this browser context.")
    }

    const rec = new SpeechRec()
    rec.continuous = true
    if (stream) {
        rec.interimResults = true
    }
    rec.lang = lang !== undefined ? lang : "en-US"

    // A single delayed restart path. Chrome's stop() is async, so calling
    // start() right after stop() throws "recognition has already started".
    // onerror and onend both funnel into this one timer instead of racing.
    let restartTimer = 0
    const scheduleRestart = () => {
        if (restartTimer) return
        restartTimer = setTimeout(() => {
            restartTimer = 0
            if (!window.__shouldRun || window.__stopRequested || window.isOffline) return
            // Stamp activity so a health check in this window never sees a stale
            // engine; the daemon would otherwise reinitialize the whole browser.
            window.__lastAudioAt = Date.now()
            window.__lastResultAt = Date.now()
            try {
                rec.start()
            } catch (e) {
                console.error(`Error restarting rec: ${e.message || e}`)
            }
        }, 500)
    }

    rec.onstart = () => {
        console.log("Listening...")
        rec.isRunning = true
        window.__lastAudioAt = Date.now()
        window.__lastResultAt = Date.now()
    }

    rec.onresult = (event) => {
        window.__lastAudioAt = Date.now()
        window.__lastResultAt = Date.now()
        const events = recognitionResultsToEvents(stream, event.resultIndex, event.results)
        for (const speechEvent of events) {
            window.onSpeechEvent(speechEvent)
        }
    }

    rec.onaudiostart = () => {
        window.__lastAudioAt = Date.now()
    }

    rec.onspeechstart = () => {
        window.__lastAudioAt = Date.now()
    }

    rec.onspeechend = () => {
        window.__lastAudioAt = Date.now()
    }

    rec.onerror = (event) => {
        if (event.error === "network") {
            window.isOffline = true
            return
        }
        // Transient errors (no speech, aborted, audio capture, permission) must
        // not kill the daemon. Defer to the scheduled restart instead of an
        // inline stop()+start(), which races Chrome's async stop().
        console.error(`recognition error: ${event.error} - scheduling restart`)
        scheduleRestart()
    }

    rec.onend = () => {
        rec.isRunning = false
        window.isOffline = undefined
        if (window.__stopRequested) {
            window.__stopRequested = false
            window.__shouldRun = false
            window.onBrowserRecStop({ reason: "silence" })
        } else if (window.isOffline) {
            window.__shouldRun = false
            window.onBrowserRecStop({ reason: "offline" })
        } else if (window.__shouldRun) {
            // Chrome ends a continuous session on its own (e.g. after a pause or
            // a foreign-language word). Restart so dictation keeps going instead
            // of shutting the whole daemon down.
            scheduleRestart()
        } else {
            window.onBrowserRecStop({ reason: "silence" })
        }
    }

    window.recognition = rec
    window.__stopRequested = false
    window.__shouldRun = false
    window.__lastAudioAt = 0
    window.__lastResultAt = 0
}

export function setStopRequested(val) {
    window.__stopRequested = !!val
    if (window.__stopRequested) {
        window.__shouldRun = false
    }
}

export function startListening() {
    if (!window.recognition) {
        const message = "rec not initialized"
        console.error(message)
        return
    }
    window.__stopRequested = false
    window.__shouldRun = true
    try {
        window.recognition.start()
    } catch (e) {
        const message = `Error starting rec: ${e.message || e}`
        console.error(message)
    }
}

export function setLangAndStart(lang) {
    if (!window.recognition) {
        console.error("rec not initialized")
        return
    }
    window.__stopRequested = false
    window.__shouldRun = true
    try {
        window.recognition.lang = lang
        window.recognition.start()
    } catch (e) {
        console.error(`Error setting lang and starting rec: ${e.message || e}`)
    }
}

export function stopRecognition() {
    if (!window.recognition) {
        const message = "rec not initialized"
        console.error(message)
        return
    }

    try {
        window.recognition.stop()
    } catch (e) {
        const message = `Error stopping rec: ${e.message || JSON.stringify(e)}`
        console.error(message)
    }
}

export function healthCheck() {
    const rec = window.recognition
    if (!rec) return "no-recognition"
    if (!rec.isRunning) return "idle"
    // A running engine that has produced no audio / results for 20s is hung,
    // not quiet: Chrome's continuous mode re-arms and fires audio events when
    // it hears something, so truly silent pauses still end the timer. This is
    // what lets the daemon's watchdog recover a dead engine without a manual
    // double-press, and makes the pill flash loading/error during recovery.
    const last = Math.max(window.__lastAudioAt || 0, window.__lastResultAt || 0)
    if (Date.now() - last > 20000) return "stalled"
    return "listening"
}
