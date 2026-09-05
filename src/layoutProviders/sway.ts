import { execSync } from "node:child_process"
import type { LayoutProvider } from "./types.js"
import { keymapToLang } from "./types.js"

export const swayProvider: LayoutProvider = {
    name: "sway",
    detect() {
        try {
            const raw = execSync("swaymsg -t get_inputs", { encoding: "utf8", timeout: 1000 })
            const inputs = JSON.parse(raw)
            for (const input of inputs ?? []) {
                if (input?.type === "keyboard") {
                    const xkb = input?.xkb_active_layout_name ?? ""
                    return keymapToLang(xkb)
                }
            }
        } catch { /* not on Sway */ }
        return null
    },
}
