const HELP_TEXT = `
EXTRA TYPE - System-Wide Dictation Daemon

Usage:
  extra-type                 Run the daemon (foreground).
  extra-type toggle          Start daemon if needed, toggle dictation.
  extra-type start           Run the daemon (foreground).
  extra-type status          Show daemon status.
  extra-type stop            Stop the daemon.
  extra-type settings        Open the settings window.
  extra-type help            Show this help.

  extra-type lang            Show current language.
  extra-type lang <bcp47>    Set language (e.g. en-US, ru-RU).
  extra-type autodetect      Show auto-detect status.
  extra-type autodetect on   Enable auto-detect keyboard layout.
  extra-type autodetect off  Disable auto-detect keyboard layout.

Configuration:
  All settings live in ~/.config/extra-type.jsonc (JSON with // comments).
  On first run a default file is written there automatically.

  {
    "port": 3232,
    "lang": "en-US",
    "browser_type": "chrome",
    "browser_path": "",
    "timeout": 0,
    "sound": false,
    "text": false,
    "punctuation": true,
    "autodetect_lang": false,
    "visualizer": { "enabled": true, "path": "" }
  }

Supported languages (33 total):
  en-US  en-GB  en-AU  en-CA  en-IN  es-ES  es-MX  es-AR  es-CO
  ru-RU  zh-CN  zh-TW  zh-HK  ja-JP  ko-KR  fr-FR  fr-CA
  de-DE  de-AT  de-CH  pt-BR  pt-PT  it-IT  nl-NL  pl-PL
  tr-TR  ar-SA  hi-IN  sv-SE  no-NO  da-DK  fi-FI  el-GR
  he-IL  th-TH  vi-VN  id-ID  uk-UA  cs-CZ  ro-RO  hu-HU

HTTP API (GET on http://localhost:<port>):
  /health  /toggle  /start  /stop  /exit
  /togglePunctuation  /setlang  /config  /open-settings
`

export function showHelp() {
    console.log(HELP_TEXT)
}
