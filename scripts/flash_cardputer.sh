#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/flash_cardputer.sh [OPTIONS]

Build the Cardputer firmware with PlatformIO and flash it to an attached device.

Options:
  -p, --port <device>   Serial device to flash (e.g. /dev/ttyACM0). Auto-detected when omitted.
  -e, --env <name>      PlatformIO environment to build (default: m5stack_cardputer).
      --no-upload       Build only; skip flashing.
  -h, --help            Show this help text and exit.

Environment variables:
  PIO_BIN   Override the PlatformIO executable to invoke (defaults to 'pio').
EOF
}

if [[ -n "${PIO_BIN:-}" ]]; then
    PIO_BIN="$PIO_BIN"
else
    if command -v pio >/dev/null 2>&1; then
        PIO_BIN="$(command -v pio)"
    elif [[ -x "$HOME/.platformio/penv/bin/platformio" ]]; then
        PIO_BIN="$HOME/.platformio/penv/bin/platformio"
    elif command -v platformio >/dev/null 2>&1; then
        PIO_BIN="$(command -v platformio)"
    else
        PIO_BIN="pio"
    fi
fi

PIO_BIN=${PIO_BIN}
PIO_ENV="m5stack_cardputer"
UPLOAD=true
PORT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)
            [[ $# -ge 2 ]] || { echo "Error: missing value for $1" >&2; usage; exit 1; }
            PORT="$2"
            shift 2
            ;;
        -e|--env)
            [[ $# -ge 2 ]] || { echo "Error: missing value for $1" >&2; usage; exit 1; }
            PIO_ENV="$2"
            shift 2
            ;;
        --no-upload)
            UPLOAD=false
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown option '$1'" >&2
            usage
            exit 1
            ;;
    esac
done

if ! "$PIO_BIN" --version >/dev/null 2>&1; then
    echo "Error: PlatformIO CLI ('$PIO_BIN') not found or not executable. Install PlatformIO or set PIO_BIN." >&2
    exit 1
fi

PIO_ARGS=("-e" "$PIO_ENV")
if [[ -n "$PORT" ]]; then
    PIO_ARGS+=("--upload-port" "$PORT")
fi

"$PIO_BIN" run "${PIO_ARGS[@]}"

if $UPLOAD; then
    "$PIO_BIN" run "${PIO_ARGS[@]}" -t upload
fi
