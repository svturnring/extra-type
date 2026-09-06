// Injected into Chrome via page.evaluate — no imports. IMPORTANT: Puppeteer only
// serializes the top-level exported function plus whatever is defined INSIDE it
// (and the closures of the nested functions). Anything declared at module scope
// (constants, helper functions) is NOT available in the page context. So all
// logic + constants used by a function that gets page.evaluate()-d must be
// defined inside that function's body. This file is the SINGLE source of truth;
// the same constants/logic are mirrored as literals inside the injected
// functions below (duplicated deliberately, not via module-scope helpers).
// Event mapper: keep browserRecognition.js in sync (tests/browserRecognition.sync.test.ts).

// Module-scope helpers exist ONLY for standalone unit tests
// (tests/browserErrors.sync.test.ts). They are NOT called from the injected
// functions — the injected functions carry their own copies (see below).
export function isFatalError(code) {
    return new Set(["not-allowed", "service-not-allowed", "audio-capture", "language-not-supported", "bad-grammar"]).has(code)
}

export function shouldEscalateRestarts(restartCount, lastRestartAt, now) {
    const MAX_RESTARTS = 3
    const RESTART_WINDOW_MS = 30000
    return restartCount >= MAX_RESTARTS && (now - lastRestartAt) < RESTART_WINDOW_MS
}

export function initWSA(stream, lang) {
    // Self-contained copies for the page context (see header comment).
    const FATAL_ERRORS = new Set([
        "not-allowed",
        "service-not-allowed",
        "audio-capture",
        "language-not-supported",
        "bad-grammar",
    ])
    const RESTART_WINDOW_MS = 30000
    const MAX_RESTARTS = 3

    const isFatalError = (code) => FATAL_ERRORS.has(code)
    const shouldEscalateRestarts = (restartCount, lastRestartAt, now) =>
        restartCount >= MAX_RESTARTS && (now - lastRestartAt) < RESTART_WINDOW_MS

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

    window.__recState = {
        running: false,
        resultAt: 0,
        lastError: null,
        lastErrorAt: 0,
        restartCount: 0,
        lastRestartAt: 0,
    }

    let restartTimer = 0
    const scheduleRestart = () => {
        if (restartTimer) return
        restartTimer = setTimeout(() => {
            restartTimer = 0
            if (!window.__shouldRun || window.__stopRequested || window.isOffline) return
            const now = Date.now()
            const rs = window.__recState
            if (shouldEscalateRestarts(rs.restartCount, rs.lastRestartAt, now)) {
                console.error(`recognition: too many restarts (${rs.restartCount}) in ${RESTART_WINDOW_MS}ms — escalating`)
                rs.running = false
                if (window.onBrowserRecError) {
                    window.onBrowserRecError({ code: "too-many-restarts", message: "Recognition restarted too many times without success" })
                }
                return
            }
            rs.lastRestartAt = now
            rs.restartCount++
            try {
                rec.start()
            } catch (e) {
                console.error(`Error restarting rec: ${e.message || e}`)
                rs.running = false
                if (window.onBrowserRecError) {
                    window.onBrowserRecError({ code: "restart-failed", message: e.message || String(e) })
                }
            }
        }, 500)
    }

    rec.onstart = () => {
        console.log("Listening...")
        rec.isRunning = true
        window.__recState.running = true
        window.__recState.restartCount = 0
        window.__lastAudioAt = Date.now()
        window.__lastResultAt = Date.now()
    }

    rec.onresult = (event) => {
        window.__lastAudioAt = Date.now()
        window.__lastResultAt = Date.now()
        window.__recState.resultAt = Date.now()
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
        const rs = window.__recState
        rs.lastError = event.error
        rs.lastErrorAt = Date.now()
        if (isFatalError(event.error)) {
            console.error(`recognition FATAL error: ${event.error}`)
            rs.running = false
            if (window.onBrowserRecError) {
                window.onBrowserRecError({ code: event.error, message: event.message || event.error })
            }
            if (window.onBrowserRecStop) {
                window.onBrowserRecStop({ reason: "error" })
            }
        } else {
            console.error(`recognition error: ${event.error} - scheduling restart`)
            scheduleRestart()
        }
    }

    rec.onend = () => {
        rec.isRunning = false
        window.__recState.running = false
        window.isOffline = undefined
        if (window.__stopRequested) {
            window.__stopRequested = false
            window.__shouldRun = false
            window.onBrowserRecStop({ reason: "silence" })
        } else if (window.isOffline) {
            window.__shouldRun = false
            window.onBrowserRecStop({ reason: "offline" })
        } else if (window.__shouldRun) {
            /* Chrome ended the continuous session on its own (pause,
             * unheard fragment). We're about to silently restart inside the
             * page — tell the daemon so it can stop diffing the stale interim,
             * or the first partial of the new session would backspace over the
             * text already dictated. */
            if (window.onBrowserRecRestart) {
                window.onBrowserRecRestart()
            }
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
    if (!rec) return { state: "no-recognition", lastError: null, restartCount: 0 }
    const rs = window.__recState || { lastError: null, restartCount: 0 }
    if (rs.lastError) return { state: "error", lastError: rs.lastError, restartCount: rs.restartCount }
    if (!rec.isRunning) return { state: "idle", lastError: null, restartCount: rs.restartCount }
    const last = Math.max(window.__lastAudioAt || 0, window.__lastResultAt || 0, rs.resultAt || 0)
    const STALL_TIMEOUT_MS = 10000
    if (Date.now() - last > STALL_TIMEOUT_MS) return { state: "stalled", lastError: null, restartCount: rs.restartCount }
    return { state: "listening", lastError: null, restartCount: rs.restartCount }
}
