import { execSync } from "node:child_process"
import type { LayoutProvider } from "./types.js"
import { keymapToLang } from "./types.js"

export const xkbProvider: LayoutProvider = {
    name: "xkb",
    detect() {
        try {
            const raw = execSync("setxkbmap -query", { encoding: "utf8", timeout: 1000 })
            for (const line of raw.split("\n")) {
                const trimmed = line.trim()
                if (trimmed.startsWith("layout:")) {
                    const layout = trimmed.split(":")[1]?.trim() ?? ""
                    return keymapToLang(layout)
                }
            }
        } catch { /* no Xwayland / setxkbmap */ }
        return null
    },
}
