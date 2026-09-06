import type { BrowserType } from "./browserLauncher.js"
import type { WSA_LANGUAGES } from "./constants.js"

export type Urgency = "low" | "normal" | "critical"

export type VizStatus = "listening" | "starting" | "recovering" | "error" | "offline" | "idle"

export type InsertMethod = "type" | "paste" | "pill" | "dotool"

export type WSALanguage = (typeof WSA_LANGUAGES)[keyof typeof WSA_LANGUAGES]

export type SpeechEvent = { kind: "text"; text: string } | { kind: "segment-finalized" }

export interface VisualizerConfig {
    enabled: boolean
    path: string
}

export interface ExtraTypeConfig {
    port: number
    lang: string
    browser_type: BrowserType
    browser_path: string
    stream: boolean
    timeout: number
    sound: boolean
    text: boolean
    punctuation: boolean
    autodetect_lang: boolean
    hotkey: string | null
    paste: string
    insert_method: InsertMethod
    visualizer: VisualizerConfig
}
