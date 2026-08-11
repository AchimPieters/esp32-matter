#!/usr/bin/env bash
#
# Generate a Matter factory partition + QR code — entirely on your own machine.
# No cloud, no data leaves your computer.
#
# It produces, under out/<vid_pid>/<uuid>/:
#   - <uuid>-partition.bin   -> flash this to the "fctry" partition (0x3E0000)
#   - <uuid>-qrcode.png      -> scan this in Google Home / Apple Home / HA / Alexa
#   - a manual pairing code  -> printed to the console / stored in the CSV
#
# For HOME / HOBBY use the built-in TEST certificates are fine (test Vendor ID).
# For a real certified product you need your own Vendor ID from the
# Connectivity Standards Alliance (CSA) and matching certificates.
#
# Prerequisite:  pip install esp-matter-mfg-tool
# Reference:     esp-matter-mfg-tool --help

set -euo pipefail

# --- Configurable values (override via environment variables) ---------------
VENDOR_ID="${VENDOR_ID:-0xFFF1}"       # 0xFFF1..0xFFF4 are reserved TEST VIDs
PRODUCT_ID="${PRODUCT_ID:-0x8000}"
VENDOR_NAME="${VENDOR_NAME:-MyHome}"
PRODUCT_NAME="${PRODUCT_NAME:-MyLight}"
DISCRIMINATOR="${DISCRIMINATOR:-3840}" # 12-bit value to tell devices apart
PASSCODE="${PASSCODE:-20202021}"       # 8-digit setup passcode
HW_VER="${HW_VER:-1}"
HW_VER_STR="${HW_VER_STR:-v1.0}"

# Inside the espressif/esp-matter image, ESP_MATTER_PATH is set; derive the
# underlying Matter SDK (connectedhomeip) path from it if not provided.
MATTER_SDK_PATH="${MATTER_SDK_PATH:-${ESP_MATTER_PATH:-}/connectedhomeip/connectedhomeip}"

# Certification Declaration for the TEST VID/PID. Adjust the filename if you use
# a different test VID/PID pair.
CD_PATH="${CD_PATH:-${MATTER_SDK_PATH}/credentials/test/certification-declaration/Chip-Test-CD-FFF1-8000.der}"

# --- Checks -----------------------------------------------------------------
if ! command -v esp-matter-mfg-tool >/dev/null 2>&1; then
    echo "ERROR: esp-matter-mfg-tool not found."
    echo "       Install it with:  pip install esp-matter-mfg-tool"
    exit 1
fi

if [ ! -f "$CD_PATH" ]; then
    echo "ERROR: Certification Declaration not found at:"
    echo "       $CD_PATH"
    echo "       Set MATTER_SDK_PATH (source tools/env.sh) or pass CD_PATH=..."
    exit 1
fi

# --- Generate ---------------------------------------------------------------
echo "Generating factory partition + QR code (TEST certificates)..."
esp-matter-mfg-tool \
    -v "$VENDOR_ID" -p "$PRODUCT_ID" \
    --vendor-name "$VENDOR_NAME" --product-name "$PRODUCT_NAME" \
    --discriminator "$DISCRIMINATOR" --passcode "$PASSCODE" \
    --hw-ver "$HW_VER" --hw-ver-str "$HW_VER_STR" \
    -cd "$CD_PATH"

echo
echo "Done. Look under ./out/ for:"
echo "  *-partition.bin  -> flash to the fctry partition:"
echo "     esptool.py -p <PORT> write_flash 0x3E0000 <uuid>-partition.bin"
echo "  *-qrcode.png     -> scan this to commission the device"
