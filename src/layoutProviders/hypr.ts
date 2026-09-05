import { execSync } from "node:child_process"
import type { LayoutProvider } from "./types.js"
import { keymapToLang } from "./types.js"

export const hyprProvider: LayoutProvider = {
    name: "hyprland",
    detect() {
        try {
            const raw = execSync("hyprctl -j devices", { encoding: "utf8", timeout: 1000 })
            const data = JSON.parse(raw)
            for (const k of data?.keyboards ?? []) {
                if (k?.main) {
                    const keymap = String(k.active_keymap ?? "")
                    return keymapToLang(keymap)
                }
            }
        } catch { /* not on Hyprland */ }
        return null
    },
}
