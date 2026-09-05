export interface LayoutProvider {
    name: string
    detect(): string | null
}

export function keymapToLang(keymap: string): string | null {
    if (/russian|ru/i.test(keymap)) return "ru-RU"
    if (/english|us|en/i.test(keymap)) return "en-US"
    if (/spanish|es/i.test(keymap)) return "es-ES"
    if (/french|fr/i.test(keymap)) return "fr-FR"
    if (/german|de/i.test(keymap)) return "de-DE"
    if (/chinese|zh/i.test(keymap)) return "zh-CN"
    if (/japanese|ja/i.test(keymap)) return "ja-JP"
    if (/korean|ko/i.test(keymap)) return "ko-KR"
    if (/portuguese|pt/i.test(keymap)) return "pt-BR"
    if (/italian|it/i.test(keymap)) return "it-IT"
    if (/dutch|nl/i.test(keymap)) return "nl-NL"
    if (/polish|pl/i.test(keymap)) return "pl-PL"
    if (/turkish|tr/i.test(keymap)) return "tr-TR"
    if (/arabic|ar/i.test(keymap)) return "ar-SA"
    if (/hindi|hi/i.test(keymap)) return "hi-IN"
    if (/swedish|sv/i.test(keymap)) return "sv-SE"
    if (/norwegian|no/i.test(keymap)) return "no-NO"
    if (/danish|da/i.test(keymap)) return "da-DK"
    if (/finnish|fi/i.test(keymap)) return "fi-FI"
    if (/greek|el/i.test(keymap)) return "el-GR"
    if (/hebrew|he/i.test(keymap)) return "he-IL"
    if (/thai|th/i.test(keymap)) return "th-TH"
    if (/vietnamese|vi/i.test(keymap)) return "vi-VN"
    if (/indonesian|id/i.test(keymap)) return "id-ID"
    if (/ukrainian|uk/i.test(keymap)) return "uk-UA"
    if (/czech|cs/i.test(keymap)) return "cs-CZ"
    if (/romanian|ro/i.test(keymap)) return "ro-RO"
    if (/hungarian|hu/i.test(keymap)) return "hu-HU"
    return null
}
