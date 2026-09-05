import { homedir } from "node:os"
import { access, readFile, rename, writeFile, mkdir } from "node:fs/promises"
import { detectDefaultBrowser } from "./browserLauncher.js"
import { DEFAULT_LANGUAGE, isValidLanguage } from "./language.js"
import { PORT } from "./constants.js"
import { log } from "./logger.js"
import type { ExtraTypeConfig } from "./types.js"

export function configFilePath(): string {
    const base = process.env.XDG_CONFIG_HOME || `${homedir()}/.config`
    return `${base}/extra-type.jsonc`
}

const DEFAULT_CONFIG: ExtraTypeConfig = {
    port: PORT,
    lang: DEFAULT_LANGUAGE,
    browser_type: "chrome",
    browser_path: "",
    stream: true,
    timeout: 0,
    sound: true,
    text: true,
    punctuation: true,
    autodetect_lang: false,
    hotkey: null,
    paste: "ctrl+v",
    insert_method: "type",
    visualizer: { enabled: true, path: "" },
}

export class ConfigParseError extends Error {
    filePath: string

    constructor(message: string, filePath: string) {
        super(message)
        this.name = "ConfigParseError"
        this.filePath = filePath
    }
}

export function stripJsoncComments(text: string): string {
    let out = ""
    let inString = false
    let escapeNext = false

    for (let i = 0; i < text.length; i++) {
        const ch = text[i]

        if (escapeNext) {
            out += ch
            escapeNext = false
            continue
        }

        if (inString) {
            out += ch
            if (ch === "\\") {
                escapeNext = true
            } else if (ch === '"') {
                inString = false
            }
            continue
        }

        if (ch === '"') {
            inString = true
            out += ch
            continue
        }

        if (ch === "/" && text[i + 1] === "/") {
            while (i < text.length && text[i] !== "\n") {
                i++
            }
            if (i < text.length) {
                out += text[i]
            }
            continue
        }

        out += ch
    }

    return out
}

export function stripTrailingCommas(text: string): string {
    let out = ""
    let inString = false
    let escapeNext = false

    for (let i = 0; i < text.length; i++) {
        const ch = text[i]

        if (escapeNext) {
            out += ch
            escapeNext = false
            continue
        }

        if (inString) {
            out += ch
            if (ch === "\\") {
                escapeNext = true
            } else if (ch === '"') {
                inString = false
            }
            continue
        }

        if (ch === '"') {
            inString = true
            out += ch
            continue
        }

        if (ch === ",") {
            let j = i + 1
            while (j < text.length && /\s/.test(text[j])) {
                j++
            }
            if (j < text.length && (text[j] === "}" || text[j] === "]")) {
                continue
            }
        }

        out += ch
    }

    return out
}

function warn(field: string, reason: string): void {
    log("CONFIG", `${field}: ${reason}`)
}

export function validateConfig(raw: unknown): ExtraTypeConfig {
    if (typeof raw !== "object" || raw === null) {
        log("CONFIG", "config file is not a JSON object")
        return { ...DEFAULT_CONFIG }
    }

    const obj = raw as Record<string, unknown>
    const result: ExtraTypeConfig = { ...DEFAULT_CONFIG }

    if (typeof obj.port === "number" && Number.isInteger(obj.port) && obj.port >= 1024 && obj.port <= 65535) {
        result.port = obj.port
    } else if ("port" in obj) {
        warn("port", `invalid value, using default ${DEFAULT_CONFIG.port}`)
    }

    if (typeof obj.lang === "string" && isValidLanguage(obj.lang)) {
        result.lang = obj.lang
    } else if ("lang" in obj) {
        warn("lang", `invalid language, using default ${DEFAULT_CONFIG.lang}`)
    }

    if (obj.browser_type === "chrome" || obj.browser_type === "chromium") {
        result.browser_type = obj.browser_type
    } else if ("browser_type" in obj) {
        warn("browser_type", `must be "chrome" or "chromium", using default ${DEFAULT_CONFIG.browser_type}`)
    }

    if (typeof obj.browser_path === "string") {
        result.browser_path = obj.browser_path
    } else if ("browser_path" in obj) {
        warn("browser_path", `must be a string, using default`)
    }

    if (typeof obj.stream === "boolean") {
        result.stream = obj.stream
    } else if ("stream" in obj) {
        warn("stream", `must be boolean, using default ${DEFAULT_CONFIG.stream}`)
    }

    if (typeof obj.timeout === "number" && Number.isInteger(obj.timeout) && obj.timeout >= 0) {
        result.timeout = obj.timeout
    } else if ("timeout" in obj) {
        warn("timeout", `must be a non-negative integer, using default ${DEFAULT_CONFIG.timeout}`)
    }

    if (typeof obj.sound === "boolean") {
        result.sound = obj.sound
    } else if ("sound" in obj) {
        warn("sound", `must be boolean, using default ${DEFAULT_CONFIG.sound}`)
    }

    if (typeof obj.text === "boolean") {
        result.text = obj.text
    } else if ("text" in obj) {
        warn("text", `must be boolean, using default ${DEFAULT_CONFIG.text}`)
    }

    if (typeof obj.punctuation === "boolean") {
        result.punctuation = obj.punctuation
    } else if ("punctuation" in obj) {
        warn("punctuation", `must be boolean, using default ${DEFAULT_CONFIG.punctuation}`)
    }

    if (typeof obj.autodetect_lang === "boolean") {
        result.autodetect_lang = obj.autodetect_lang
    } else if ("autodetect_lang" in obj) {
        warn("autodetect_lang", `must be boolean, using default ${DEFAULT_CONFIG.autodetect_lang}`)
    }

    if (typeof obj.hotkey === "string" && obj.hotkey.length > 0 && obj.hotkey.length <= 200) {
        result.hotkey = obj.hotkey
    } else if ("hotkey" in obj && obj.hotkey !== null && obj.hotkey !== "") {
        warn("hotkey", `must be a string or null, using default (disabled)`)
    }

    if (typeof obj.paste === "string" && obj.paste.length > 0 && obj.paste.length <= 200) {
        result.paste = obj.paste
    } else if ("paste" in obj && obj.paste !== "") {
        warn("paste", `must be a non-empty string, using default ${DEFAULT_CONFIG.paste}`)
    }

    if (obj.insert_method === "type" || obj.insert_method === "paste" || obj.insert_method === "pill") {
        result.insert_method = obj.insert_method
    } else if ("insert_method" in obj) {
        warn("insert_method", `must be "type", "paste" or "pill", using default ${DEFAULT_CONFIG.insert_method}`)
    }

    if (typeof obj.visualizer === "object" && obj.visualizer !== null) {
        const v = obj.visualizer as Record<string, unknown>
        if (typeof v.enabled === "boolean") result.visualizer.enabled = v.enabled
        if (typeof v.path === "string") result.visualizer.path = v.path
    }

    return result
}

function hasAllFieldsInvalid(raw: unknown): boolean {
    if (typeof raw !== "object" || raw === null) return true
    const obj = raw as Record<string, unknown>
    const fields = [
        "port",
        "lang",
        "browser_type",
        "browser_path",
        "stream",
        "timeout",
        "sound",
        "text",
        "punctuation",
        "autodetect_lang",
        "hotkey",
        "paste",
        "insert_method",
        "visualizer",
    ]
    const validated = validateConfig(raw)
    for (const f of fields) {
        if (obj[f] !== undefined) {
            const vk = f as keyof ExtraTypeConfig
            if (JSON.stringify(obj[f]) === JSON.stringify(validated[vk])) {
                return false
            }
        }
    }
    return true
}

async function generateDefaultConfig(): Promise<ExtraTypeConfig> {
    const config = { ...DEFAULT_CONFIG }

    const detected = await detectDefaultBrowser()
    if (detected) {
        config.browser_path = detected.path
        config.browser_type = detected.type
    }

    const langEnv = (process.env.LANG ?? "").split(".")[0].replace(/_/g, "-")
    if (langEnv && isValidLanguage(langEnv)) {
        config.lang = langEnv
    }

    return config
}

export async function persistConfig(cfg: ExtraTypeConfig): Promise<void> {
    const filePath = configFilePath()
    const dir = filePath.substring(0, filePath.lastIndexOf("/"))
    await mkdir(dir, { recursive: true, mode: 0o700 })
    const content = JSON.stringify(cfg, null, 4) + "\n"
    const tmp = filePath + ".tmp"
    await writeFile(tmp, content, { encoding: "utf8", mode: 0o600 })
    await rename(tmp, filePath)
}

async function backupIfMissing(filePath: string): Promise<void> {
    const bakPath = filePath + ".bak"
    try {
        await access(bakPath)
    } catch (err: any) {
        if (err.code === "ENOENT") {
            await rename(filePath, bakPath)
        }
    }
}

export async function loadConfig(): Promise<ExtraTypeConfig> {
    const filePath = configFilePath()

    /* migrate from the pre-renaming path if the new one doesn't exist yet */
    {
        const base = process.env.XDG_CONFIG_HOME || `${homedir()}/.config`
        const oldPath = `${base}/voice-type.jsonc`
        try {
            await access(filePath)
        } catch {
            try {
                await access(oldPath)
                await rename(oldPath, filePath)
                log("CONFIG", `migrated ${oldPath} -> ${filePath}`)
            } catch {
                /* no old config — fine */
            }
        }
    }

    let rawText: string
    try {
        rawText = await readFile(filePath, "utf8")
    } catch (err: any) {
        if (err.code === "ENOENT") {
            const config = await generateDefaultConfig()
            await persistConfig(config)
            log("CONFIG", "CLI flags moved to ~/.config/extra-type.jsonc — a default config has been written there.")
            return config
        }
        log("CONFIG", `could not read config file: ${(err as Error).message}`)
        return await generateDefaultConfig()
    }

    let parsed: unknown
    try {
        const stripped = stripTrailingCommas(stripJsoncComments(rawText))
        parsed = JSON.parse(stripped)
    } catch (err: any) {
        if (err instanceof ConfigParseError) {
            log("CONFIG", err.message)
        } else {
            log("CONFIG", `could not parse config file: ${(err as Error).message}`)
        }
        await backupIfMissing(filePath)
        const defaultCfg = await generateDefaultConfig()
        await persistConfig(defaultCfg)
        return defaultCfg
    }

    const validated = validateConfig(parsed)

    if (hasAllFieldsInvalid(parsed)) {
        log("CONFIG", "all config fields are invalid — backing up and writing default")
        await backupIfMissing(filePath)
        const defaultCfg = await generateDefaultConfig()
        await persistConfig(defaultCfg)
        return defaultCfg
    }

    return validated
}
