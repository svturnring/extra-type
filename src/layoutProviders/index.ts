import { log } from "../logger.js"
import type { LayoutProvider } from "./types.js"
import { hyprProvider } from "./hypr.js"
import { swayProvider } from "./sway.js"
import { xkbProvider } from "./xkb.js"

const providers: LayoutProvider[] = [
    hyprProvider,
    swayProvider,
    xkbProvider,
]

let activeProvider: LayoutProvider | null = null

export function detectLanguage(): string | null {
    for (const p of providers) {
        const result = p.detect()
        if (result) {
            if (!activeProvider || activeProvider.name !== p.name) {
                log("LAYOUT", `detected via ${p.name}`)
                activeProvider = p
            }
            return result
        }
    }
    return null
}

export function getActiveProvider(): string | null {
    return activeProvider?.name ?? null
}
