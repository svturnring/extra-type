import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

function extractFnBody(source: string, name: string): string {
    const match = source.match(new RegExp(`export function ${name}\\([^)]*\\) \\{([\\s\\S]*?)\\n\\}`))
    if (!match) throw new Error(`could not extract ${name} from browser.js`)
    return match[1]!
}

function extractFatalSet(source: string): string {
    const match = source.match(/const FATAL_ERRORS = new Set\(\[([\s\S]*?)\]\)/)
    if (!match) throw new Error("could not extract FATAL_ERRORS from browser.js")
    return `new Set([${match[1]}])`
}

function loadBrowserHelpers(): {
    isFatalError: (c: string) => boolean
    shouldEscalateRestarts: (rc: number, last: number, now: number) => boolean
} {
    const src = readFileSync(join(import.meta.dir, "../src/browser.js"), "utf8")
    const maxRestarts = src.match(/const MAX_RESTARTS = (\d+)/)?.[1] ?? "3"
    const restartWindow = src.match(/const RESTART_WINDOW_MS = (\d+)/)?.[1] ?? "30000"
    const factory = new Function(`
        const FATAL_ERRORS = ${extractFatalSet(src)};
        const MAX_RESTARTS = ${maxRestarts};
        const RESTART_WINDOW_MS = ${restartWindow};
        function isFatalError(code) { ${extractFnBody(src, "isFatalError")} }
        function shouldEscalateRestarts(restartCount, lastRestartAt, now) { ${extractFnBody(src, "shouldEscalateRestarts")} }
        return { isFatalError, shouldEscalateRestarts };
    `)
    return factory()
}

describe("browser.js error classification (isFatalError)", () => {
    const { isFatalError } = loadBrowserHelpers()

    test("fatal codes are NOT auto-recoverable", () => {
        expect(isFatalError("not-allowed")).toBe(true)
        expect(isFatalError("service-not-allowed")).toBe(true)
        expect(isFatalError("audio-capture")).toBe(true)
        expect(isFatalError("language-not-supported")).toBe(true)
        expect(isFatalError("bad-grammar")).toBe(true)
    })

    test("transient codes ARE auto-recoverable", () => {
        expect(isFatalError("no-speech")).toBe(false)
        expect(isFatalError("aborted")).toBe(false)
        expect(isFatalError("unknown")).toBe(false)
    })
})

describe("browser.js restart escalation (shouldEscalateRestarts)", () => {
    const { shouldEscalateRestarts } = loadBrowserHelpers()
    const now = 1_000_000

    test("below cap → no escalation", () => {
        expect(shouldEscalateRestarts(2, now - 1000, now)).toBe(false)
    })

    test("at cap within window → escalate", () => {
        expect(shouldEscalateRestarts(3, now - 1000, now)).toBe(true)
    })

    test("at cap but outside window → no escalation", () => {
        expect(shouldEscalateRestarts(3, now - 40000, now)).toBe(false)
    })
})
