# Security & privacy

This project is designed so that **nothing about your devices is shared with any
third party**. This document explains why, and how to harden a device before you
rely on it.

## Privacy by design

- **Local-first protocol.** Matter commissioning happens over Bluetooth LE plus
  your local network; ongoing control runs over your LAN (Wi-Fi or Thread).
  There is no vendor cloud in the loop.
- **No telemetry.** The firmware here is plain esp-matter code. It does not phone
  home. You can read every line.
- **Local QR generation.** `tools/gen_factory.sh` runs entirely on your machine.
  Certificates and commissioning data are generated offline.
- **Your data stays yours.** If you pair the device with Home Assistant, control
  never leaves your network. If you pair it with Google/Apple/Alexa instead,
  those hubs apply their own cloud policies — that's a choice you make per
  controller, not something this firmware forces.

## Certificates

- For **home / hobby** use, the built-in Matter **test** certificates and a test
  Vendor ID (`0xFFF1`–`0xFFF4`) are fine. Everything works locally.
- For a **certified commercial product**, you need your own Vendor ID and
  production certificates from the Connectivity Standards Alliance (CSA), plus
  formal interop testing. That is a separate, paid process.

## Hardening before deployment

While you're learning, keep these **off** — they make re-flashing harder and can
permanently lock a board if misused. Turn them on once your firmware is stable.

### 1. Encrypt the factory partition

`gen_factory.sh` can produce an encrypted factory partition. Pass the `-e`
option to `esp-matter-mfg-tool`; it emits a separate key partition. Read the
tool's docs first — encrypted NVS is flashed differently from normal partitions.

### 2. Flash encryption

Prevents anyone with physical access from reading firmware or keys off the chip.
Enable in `sdkconfig.defaults`:

```
CONFIG_SECURE_FLASH_ENC_ENABLED=y
```

⚠️ Flash encryption burns eFuses and is effectively one-way. Read the ESP-IDF
"Flash Encryption" guide fully before enabling it on real hardware.

### 3. Secure boot

Ensures the chip only runs firmware you signed:

```
CONFIG_SECURE_BOOT=y
CONFIG_SECURE_BOOT_V2_ENABLED=y
```

Keep your signing key offline and backed up. If you lose it you can't push
updates to secure-boot devices.

### 4. Signed OTA updates

All three firmware types now ship with Matter's OTA Requestor cluster
enabled (`CONFIG_ENABLE_OTA_REQUESTOR=y` — see CLAUDE.md's open next steps
for what's done vs. still open). It's currently unsigned: any OTA Provider
a device is bound to can push it a new image, over BDX (Matter's own
transfer protocol — not a raw HTTP fetch, so "serve the image from a URL
you control" isn't quite how this works; the image comes from whatever OTA
Provider node the device is bound to on its fabric). Before relying on
this for anything beyond your own LAN: sign your OTA images and verify
signatures on the device via `esp_matter_ota_requestor_encrypted_init()`
(see `examples/light/main/app_main.cpp` in the esp-matter SDK for the
reference pattern) so only your firmware is ever accepted.

## Reporting issues

Security reports for this repository can be raised via the GitHub issue tracker
at <https://github.com/AchimPieters/esp32-matter/issues>, or through the contact
details on <https://www.studiopieters.nl/>. For anything sensitive, prefer a
private channel over a public issue.
