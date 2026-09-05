import { log } from "./logger.js"
import { detectLanguage } from "./layoutProviders/index.js"

export interface LayoutWatcher {
    start(): void
    stop(): void
}

export function createLayoutWatcher(onLang: (lang: string) => void): LayoutWatcher {
    let timer: ReturnType<typeof setInterval> | null = null
    let lastLang: string | null = null

    const poll = () => {
        const lang = detectLanguage()
        if (!lang || lang === lastLang) return
        lastLang = lang
        log("LAYOUT", `detected -> ${lang}`)
        onLang(lang)
    }

    return {
        start() {
            if (timer) return
            poll()
            timer = setInterval(poll, 500)
        },
        stop() {
            if (timer) {
                clearInterval(timer)
                timer = null
            }
        },
    }
}
