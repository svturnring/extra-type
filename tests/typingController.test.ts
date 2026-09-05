import { describe, expect, test } from "bun:test"
import TypingController from "../src/typingController.js"
import { CapturingSink } from "./helpers.js"

describe("TypingController", () => {
    test("types full text when prevText is empty", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.applyLiveText("hello")
        expect(sink.writes.join("")).toContain("type hello\n")
    })

    test("prefix diff appends suffix", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.applyLiveText("he")
        tc.applyLiveText("hello")
        const script = sink.writes.join("")
        expect(script).toContain("type he\n")
        expect(script).toContain("type llo\n")
        expect(script).not.toContain("BackSpace")
    })

    test("prefix diff backspaces changed suffix", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.applyLiveText("hello")
        tc.applyLiveText("help")
        const script = sink.writes.join("")
        expect(script).toContain("key BackSpace")
        expect(script).toContain("type p\n")
    })

    test("no-op when text unchanged", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.applyLiveText("hello")
        const count = sink.writes.length
        tc.applyLiveText("hello")
        expect(sink.writes.length).toBe(count)
    })

    test("sendBackspaces emits BackSpace keys", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.sendBackspaces(2)
        expect(sink.writes.join("")).toBe("key BackSpace \nkey BackSpace \n")
    })

    test("newline in text becomes key enter", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.typeText("a\nb")
        const script = sink.writes.join("")
        expect(script).toContain("type a\n")
        expect(script).toContain("key enter\n")
        expect(script).toContain("type b\n")
    })

    test("unicode is pasted verbatim (clipboard)", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.typeText("é")
        const script = sink.writes.join("")
        expect(script).toContain("type é\n")
        expect(script).not.toContain("ctrl+shift+u")
    })

    test("sendKeyChord", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.sendKeyChord("ctrl+enter")
        expect(sink.writes.join("")).toBe("key ctrl+enter\n")
    })

    test("hasStopped blocks typing and chords", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.hasStopped = true
        tc.applyLiveText("hello")
        tc.sendKeyChord("ctrl+enter")
        expect(sink.writes).toHaveLength(0)
    })

    test("finalizeSegment clears prevText for next apply", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.applyLiveText("hello")
        tc.finalizeSegment()
        tc.applyLiveText("world")
        const script = sink.writes.join("")
        expect(script).toContain("type hello\n")
        expect(script).toContain("type world\n")
        expect(script).not.toContain("BackSpace")
    })

    test("setInsertMethod with injected sink keeps capturing sink (no real spawn)", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink)
        tc.setInsertMethod("paste")
        tc.setInsertMethod("type")
        tc.setInsertMethod("bogus" as never)
        tc.applyLiveText("hello")
        expect(sink.writes.join("")).toContain("type hello\n")
    })

    test("pill mode collects transcript through listener, never types", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink, "ctrl+v", "pill")
        const seen: string[] = []
        tc.setPillListener((t) => seen.push(t))
        tc.applyLiveText("hello")
        tc.applyLiveText("hello world")
        tc.finalizeSegment()
        tc.applyLiveText("next")
        expect(sink.writes.join("")).toBe("")
        /* the API already spaces the text — segments are appended verbatim */
        expect(seen).toEqual(["hello", "hello world", "hello world", "hello worldnext"])
    })

    test("pill segments append verbatim, getPillText returns full text", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink, "ctrl+v", "pill")
        tc.applyLiveText("first ")
        tc.finalizeSegment()
        tc.applyLiveText("second")
        expect(tc.getPillText()).toBe("first second")
        tc.finalizeSegment()
        expect(tc.getPillText()).toBe("first second")
    })

    test("pill clear resets and notifies empty", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink, "ctrl+v", "pill")
        const seen: string[] = []
        tc.setPillListener((t) => seen.push(t))
        tc.applyLiveText("abc")
        tc.finalizeSegment()
        tc.clearPill()
        tc.applyLiveText("x")
        expect(seen).toEqual(["abc", "abc", "", "x"])
    })

    test("pill reset() after stop discards accumulated text (new dictation starts clean)", () => {
        const sink = new CapturingSink()
        const tc = new TypingController(sink, "ctrl+v", "pill")
        tc.applyLiveText("old phrase")
        tc.finalizeSegment()
        tc.hasStopped = true
        tc.reset()
        tc.hasStopped = false
        tc.applyLiveText("new")
        tc.finalizeSegment()
        expect(tc.getPillText()).toBe("new")
    })
})
