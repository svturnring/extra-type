#!/bin/sh
# extra-type uninstaller.
#
#   sh uninstall.sh
set -eu

PREFIX="/usr/local"
BINDIR="$PREFIX/bin"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log_info()  { printf '%s[INFO]%s  %s\n'  "$GREEN" "$NC" "$1" >&2; }
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

as_root() {
    if [ "$(id -u)" = "0" ]; then "$@"
    else sudo "$@"
    fi
}

remove_binaries() {
    REMOVED=0
    for f in "$BINDIR/extra-type" "$BINDIR/extra-type-viz" "$BINDIR/extra-type-settings" "$BINDIR/extra-type-hotkey"; do
        [ -e "$f" ] || continue
        case "$(prompt_yn "Remove $f?" "Y")" in
            [Yy]*) as_root rm -f "$f"; REMOVED=1 ;;
        esac
    done
    [ "$REMOVED" = "1" ] && log_info "Binaries removed."
}

remove_config() {
    CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}"
    LOG_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/extra-type"
    STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/extra-type"

    for target in "$CONFIG_DIR/extra-type.jsonc" "$CONFIG_DIR/extra-type.jsonc.bak"; do
        [ -f "$target" ] || continue
        case "$(prompt_yn "Remove $target?" "Y")" in
            [Yy]*) rm -f "$target" ;;
        esac
    done

    for dir in "$LOG_DIR" "$STATE_DIR"; do
        [ -d "$dir" ] || continue
        case "$(prompt_yn "Remove $dir?" "Y")" in
            [Yy]*) rm -rf "$dir" ;;
        esac
    done
}

main() {
    log_info "extra-type uninstaller"
    remove_binaries
    remove_config
    log_info "Done."
}

main
