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
# For HOME / HOBBY use the built-in TEST Certification Declaration is fine
# (test Vendor ID), but the device attestation chain (PAA/PAI/DAC — proves
# "this chip is genuine" during commissioning) still has to exist: this
# script generates its own self-signed test PAA the first time it runs (see
# "Device attestation" below) and reuses it after that.
#
# Reality check on controllers: Home Assistant and chip-tool commission
# self-signed test devices like this without complaint. Apple Home and
# Google Home, in their normal consumer flow, validate the attestation
# chain against their own bundled list of CSA-recognised root certificates
# — a locally-generated test PAA will never be on that list, so pairing
# will likely still fail there (a different, expected failure — "we don't
# trust your PAA" — from the bug this script fixes, which was "no PAA at
# all"). That's an ecosystem-level restriction, not something a QR code can
# work around; see SECURITY.md.
#
# For a real certified product you need your own Vendor ID from the
# Connectivity Standards Alliance (CSA) and matching certificates.
#
# Prerequisite:  pip install esp-matter-mfg-tool  (chip-cert ships with the
#                espressif/esp-matter Docker image already)
# Reference:     esp-matter-mfg-tool --help

set -euo pipefail

# --- Configurable values (override via environment variables) ---------------
VENDOR_ID="${VENDOR_ID:-0xFFF2}"       # 0xFFF1..0xFFF4 are reserved TEST VIDs.
                                        # 0xFFF2 specifically because that's
                                        # one of the two (FFF2/FFF3) the SDK
                                        # actually ships a signed test
                                        # Certification Declaration for as of
                                        # esp-matter release-v1.6 — FFF1 has
                                        # none, despite being the "classic"
                                        # example VID quoted everywhere.
PRODUCT_ID="${PRODUCT_ID:-0x8001}"
VENDOR_NAME="${VENDOR_NAME:-MyHome}"
PRODUCT_NAME="${PRODUCT_NAME:-MyLight}"
DISCRIMINATOR="${DISCRIMINATOR:-3840}" # 12-bit value to tell devices apart
PASSCODE="${PASSCODE:-20202021}"       # 8-digit setup passcode
HW_VER="${HW_VER:-1}"
HW_VER_STR="${HW_VER_STR:-v1.0}"

# Inside the espressif/esp-matter image, ESP_MATTER_PATH is set; derive the
# underlying Matter SDK (connectedhomeip) path from it if not provided.
MATTER_SDK_PATH="${MATTER_SDK_PATH:-${ESP_MATTER_PATH:-}/connectedhomeip/connectedhomeip}"

# Certification Declaration for the TEST VID/PID — computed from VENDOR_ID /
# PRODUCT_ID (not hardcoded), so overriding those env vars picks the right
# file automatically instead of silently keeping the default's CD. Only a
# few VID/PID combos actually have a signed test CD shipped in the SDK; list
# what's available with:
#   ls "$MATTER_SDK_PATH/credentials/test/certification-declaration/"
VID_HEX="${VENDOR_ID#0x}"
PID_HEX="${PRODUCT_ID#0x}"
CD_PATH="${CD_PATH:-${MATTER_SDK_PATH}/credentials/test/certification-declaration/Chip-Test-CD-${VID_HEX}-${PID_HEX}.der}"

# Device attestation: esp-matter-mfg-tool needs an existing PAA (root)
# certificate + key to derive a PAI and DAC from — it will NOT generate one
# on its own, and without a DAC/PAI the device can't answer a controller's
# CertificateChainRequest during commissioning at all (fails outright,
# regardless of which app you use). The SDK does ship test PAA certs under
# connectedhomeip/credentials/test/attestation/, but only as deliberately
# time-limited fixtures for CI (e.g. Chip-Test-PAA-FFF2-ValInPast-Cert.pem
# expired in 2022) — not meant for actually pairing a device today. So:
# generate our own long-lived one instead, once, and reuse it afterwards.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAA_DIR="${PAA_DIR:-${SCRIPT_DIR}/test-credentials}"
PAA_CERT="${PAA_CERT:-${PAA_DIR}/paa-${VID_HEX}-cert.pem}"
PAA_KEY="${PAA_KEY:-${PAA_DIR}/paa-${VID_HEX}-key.pem}"

# --- Checks -----------------------------------------------------------------
if ! command -v esp-matter-mfg-tool >/dev/null 2>&1; then
    echo "ERROR: esp-matter-mfg-tool not found."
    echo "       Install it with:  pip install esp-matter-mfg-tool"
    exit 1
fi

if [ ! -f "$CD_PATH" ]; then
    echo "ERROR: Certification Declaration not found at:"
    echo "       $CD_PATH"
    echo "       No test CD is shipped for VENDOR_ID=$VENDOR_ID PRODUCT_ID=$PRODUCT_ID."
    echo "       Available VID/PID combos:"
    ls "${MATTER_SDK_PATH}/credentials/test/certification-declaration/" 2>/dev/null \
        | grep -E '^Chip-Test-CD-[0-9A-F]+-[0-9A-F]+\.der$' \
        | sed 's/^/         /'
    echo "       Pick one of those, or pass CD_PATH=... directly."
    exit 1
fi

if [ ! -f "$PAA_CERT" ] || [ ! -f "$PAA_KEY" ]; then
    if ! command -v chip-cert >/dev/null 2>&1; then
        echo "ERROR: chip-cert not found (needed to generate a test PAA)."
        echo "       It ships with the espressif/esp-matter Docker image; run this"
        echo "       script inside tools/dev.sh, or install connectedhomeip's tools."
        exit 1
    fi
    echo "No cached test PAA for VID ${VID_HEX} yet — generating one (once) at:"
    echo "  $PAA_CERT"
    mkdir -p "$PAA_DIR"
    chip-cert gen-att-cert -t a \
        -c "esp32-matter test PAA (VID ${VID_HEX})" \
        -V "$VID_HEX" \
        -l 4294967295 \
        -o "$PAA_CERT" -O "$PAA_KEY"
fi

# --- Generate ---------------------------------------------------------------
echo "Generating factory partition + QR code (TEST certificates)..."
esp-matter-mfg-tool \
    -v "$VENDOR_ID" -p "$PRODUCT_ID" \
    --vendor-name "$VENDOR_NAME" --product-name "$PRODUCT_NAME" \
    --discriminator "$DISCRIMINATOR" --passcode "$PASSCODE" \
    --hw-ver "$HW_VER" --hw-ver-str "$HW_VER_STR" \
    -cd "$CD_PATH" \
    --paa -c "$PAA_CERT" -k "$PAA_KEY"

echo
echo "Done. Look under ./out/ for:"
echo "  *-partition.bin  -> flash to the fctry partition:"
echo "     esptool.py -p <PORT> write_flash 0x3E0000 <uuid>-partition.bin"
echo "  *-qrcode.png     -> scan this to commission the device"
