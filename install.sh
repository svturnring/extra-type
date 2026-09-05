#!/bin/sh
# extra-type installer — builds from source and installs.
#
#   curl -sSL <url> | sh
#   sh install.sh [--local]
set -eu

BINARY_NAME="extra-type"
PREFIX="/usr/local"
BINDIR="$PREFIX/bin"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info()  { printf '%s[INFO]%s  %s\n'  "$GREEN" "$NC" "$1" >&2; }
log_warn()  { printf '%s[WARN]%s  %s\n'  "$YELLOW" "$NC" "$1" >&2; }
log_error() { printf '%s[ERROR]%s %s\n'  "$RED" "$NC" "$1" >&2; }

has_tty() {
    ( true < /dev/tty ) 2>/dev/null || return 1
    ( true > /dev/tty ) 2>/dev/null
}

prompt_yn() {
    _msg="$1"; _default="$2"
    if ! has_tty; then echo "$_default"; return 0; fi
    case "$_default" in
        [Yy]*) _opts="Y/n" ;;
        *) _opts="y/N" ;;
    esac
    printf '%s (%s) ' "$_msg" "$_opts" > /dev/tty
    read -r _answer < /dev/tty || _answer=""
    [ -z "$_answer" ] && _answer="$_default"
    echo "$_answer"
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        log_error "'$1' is required but not installed."
        exit 1
    }
}

as_root() {
    if [ "$(id -u)" = "0" ]; then "$@"
    else sudo "$@"
    fi
}

detect_arch() {
    case "$(uname -m)" in
        x86_64|amd64)  echo "x64" ;;
        aarch64|arm64) echo "arm64" ;;
        *) log_error "Unsupported architecture: $(uname -m)"; exit 1 ;;
    esac
}

detect_pkg_manager() {
    if command -v apt-get >/dev/null 2>&1; then echo "apt"; return; fi
    if command -v dnf >/dev/null 2>&1;      then echo "dnf"; return; fi
    if command -v pacman >/dev/null 2>&1;    then echo "pacman"; return; fi
    if command -v apk >/dev/null 2>&1;       then echo "apk"; return; fi
    if command -v xbps-install >/dev/null 2>&1; then echo "xbps"; return; fi
    echo "none"
}

detect_browser() {
    for p in \
        /usr/bin/google-chrome-stable \
        /usr/bin/google-chrome \
        /opt/google/chrome/chrome \
        /usr/bin/chromium \
        /usr/bin/chromium-browser \
        /usr/local/bin/chromium
    do
        [ -x "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

check_build_deps() {
    MISSING=""
    for cmd in bun gcc pkg-config; do
        command -v "$cmd" >/dev/null 2>&1 || MISSING="$MISSING $cmd"
    done

    if ! pkg-config --exists gtk4 gtk4-layer-shell-0 libpipewire-0.3 libspa-0.2 2>/dev/null; then
        MISSING="$MISSING gtk4-layer-shell-dev pipewire-dev"
    fi

    if [ -n "$MISSING" ]; then
        log_warn "Missing build deps:$MISSING"
        PM=$(detect_pkg_manager)
        case "$PM" in
            apt)
                log_info "Installing with: sudo apt-get install -y bun gcc pkg-config libgtk-4-dev libgtk4-layer-shell-dev libpipewire-0.3-dev"
                log_info "  (install bun from https://bun.sh if not in apt)"
                ;;
            dnf)
                log_info "Installing with: sudo dnf install -y gcc pkgconf gtk4-devel gtk4-layer-shell-devel pipewire-devel"
                log_info "  (install bun from https://bun.sh if not in dnf)"
                ;;
            pacman)
                log_info "Installing with: sudo pacman -S --noconfirm gcc pkgconf gtk4 gtk4-layer-shell pipewire"
                log_info "  (install bun from AUR or https://bun.sh)"
                ;;
            apk)
                log_info "Install bun, gcc, pkgconf, gtk4-dev, gtk4-layer-shell-dev, pipewire-dev manually"
                ;;
            *)
                log_error "Cannot auto-install. Install manually: bun, gcc, pkg-config, gtk4-dev, gtk4-layer-shell-dev, pipewire-dev"
                ;;
        esac
        case "$(prompt_yn "Attempt to install build deps automatically?" "N")" in
            [Yy]*)
                case "$PM" in
                    apt)
                        as_root apt-get install -y gcc pkg-config libgtk-4-dev libgtk4-layer-shell-dev \
                            libpipewire-0.3-dev libsoup-3.0-dev libjson-glib-dev wtype wl-clipboard ;;
                    dnf)
                        as_root dnf install -y gcc pkgconf gtk4-devel gtk4-layer-shell-devel \
                            pipewire-devel libsoup3-devel json-glib-devel wtype wl-clipboard ;;
                    pacman)
                        as_root pacman -S --noconfirm gcc pkgconf gtk4 gtk4-layer-shell pipewire \
                            libsoup3 json-glib wtype wl-clipboard ;;
                    apk)
                        as_root apk add --no-cache gcc pkgconf gtk4-dev gtk4-layer-shell-dev \
                            pipewire-dev libsoup3-dev json-glib-dev wtype wl-clipboard ;;
                    xbps)
                        as_root xbps-install -y gcc pkg-config gtk4-devel gtk4-layer-shell-devel \
                            pipewire-devel libsoup3-devel json-glib-devel wtype wl-clipboard ;;
                    *)      log_error "Cannot auto-install on this distro"; exit 1 ;;
                esac
                # bun
                if ! command -v bun >/dev/null 2>&1; then
                    log_info "Installing bun..."
                    curl -fsSL https://bun.sh/install | bash
                    export PATH="$HOME/.bun/bin:$PATH"
                fi
                ;;
            *)
                log_error "Build deps required. Install them and re-run."
                exit 1
                ;;
        esac
    fi
}

check_runtime_deps() {
    for cmd in wtype wl-copy; do
        command -v "$cmd" >/dev/null 2>&1 || {
            log_warn "'$cmd' not found — typing may not work. Install it from your package manager."
        }
    done

    BROWSER_PATH=$(detect_browser)
    if [ -z "$BROWSER_PATH" ]; then
        log_warn "No Chrome/Chromium found. Set browser_path in ~/.config/extra-type.jsonc"
    else
        log_info "Browser: $BROWSER_PATH"
    fi
}

do_build() {
    log_info "Building daemon..."
    make daemon
    log_info "Building visualizer..."
    make viz
    log_info "Build complete."
}

do_install() {
    log_info "Installing to $BINDIR..."
    make install PREFIX="$PREFIX"
    log_info "Installed: $BINARY_NAME, extra-type-viz, extra-type-settings"
}

write_config() {
    CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}"
    CONFIG_FILE="$CONFIG_DIR/extra-type.jsonc"
    OLD_CONFIG_FILE="$CONFIG_DIR/voice-type.jsonc"

    # migrate pre-rename config if present
    if [ ! -e "$CONFIG_FILE" ] && [ -e "$OLD_CONFIG_FILE" ]; then
        mv "$OLD_CONFIG_FILE" "$CONFIG_FILE"
        log_info "Migrated $OLD_CONFIG_FILE -> $CONFIG_FILE"
    fi

    if [ -f "$CONFIG_FILE" ]; then
        case "$(prompt_yn "Keep existing config at $CONFIG_FILE?" "Y")" in
            [Yy]*) log_info "Keeping existing config"; return 0 ;;
        esac
    fi

    LANG_HEURISTIC="en-US"
    if [ -n "${LANG:-}" ]; then
        RAW_LANG=$(echo "$LANG" | cut -d. -f1 | tr '_' '-')
        case "$RAW_LANG" in
            en-US|en-GB|en-AU|en-CA|en-IN|es-ES|es-MX|es-AR|es-CO|ru-RU|\
            zh-CN|zh-TW|zh-HK|ja-JP|ko-KR|fr-FR|fr-CA|de-DE|de-AT|de-CH|\
            pt-BR|pt-PT|it-IT|nl-NL|pl-PL|tr-TR|ar-SA|hi-IN|sv-SE|no-NO|\
            da-DK|fi-FI|el-GR|he-IL|th-TH|vi-VN|id-ID|uk-UA|cs-CZ|ro-RO|hu-HU)
                LANG_HEURISTIC="$RAW_LANG" ;;
        esac
    fi

    mkdir -p "$CONFIG_DIR"
    cat > "$CONFIG_FILE" << EOF
{
    "port": 3232,
    "lang": "${LANG_HEURISTIC}",
    "browser_type": "chrome",
    "browser_path": "$(detect_browser 2>/dev/null || echo "")",
    "timeout": 0,
    "sound": true,
    "punctuation": true,
    "autodetect_lang": false,
    "visualizer": { "enabled": true, "path": "" }
}
EOF
    chmod 600 "$CONFIG_FILE"
    log_info "Wrote $CONFIG_FILE"
}

print_summary() {
    log_info "extra-type installed."
    log_info ""
    log_info "Start the daemon:  extra-type"
    log_info "Toggle dictation:  extra-type toggle"
    log_info "Open settings:     extra-type settings"
    log_info "Stop daemon:       extra-type stop"
    log_info ""
    log_info "Config: ~/.config/extra-type.jsonc"
    log_info ""
    log_info "Keyboard shortcuts (set in your DE):"
    log_info "  Start/stop:  sh -c 'curl -s http://localhost:3232/exit || extra-type'"
    log_info "  Dictate:     curl -s http://localhost:3232/toggle"
}

main() {
    ARCH=$(detect_arch)
    log_info "Detected: $ARCH"

    check_build_deps
    check_runtime_deps
    do_build
    do_install
    write_config
    print_summary
}

main
