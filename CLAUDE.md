# CLAUDE.md — project context for Claude Code

This file is read automatically at the start of a Claude Code session. It captures
the intent and conventions for this repository so any session can pick up where we
left off.

## What this project is

`esp32-matter` — a fully transparent toolkit for building **Matter** smart-home
devices on ESP32 chips. Tagline: "ESP-ZeroCode Modules for Out-of-the-Box Matter
Connectivity" (AchimPieters / StudioPieters).

Core principles (do not violate these):
- **No hidden code.** Everything is plain, readable C++/shell/YAML built on the
  open-source [esp-matter](https://github.com/espressif/esp-matter) SDK. No
  prebuilt/obfuscated framework blobs.
- **No cloud, no data sharing, no telemetry.** Matter is local-first: pairing
  over BLE + LAN, control over the local network. Never add code that phones home.
- **Local QR generation.** Commissioning data + QR are generated offline.
- License: **MIT**.

## Development environment (Docker-first)

We use the Docker-based ESP-IDF workflow from
<https://www.studiopieters.nl/esp32-homekit-development/>, adapted for Matter.
The HomeKit guide uses the `espressif/idf` image, which does NOT contain
esp-matter — so we use the **`espressif/esp-matter`** image instead (ESP-IDF +
esp-matter pre-installed, `IDF_PATH` and `ESP_MATTER_PATH` already set).

Pinned to **`espressif/esp-matter:release-v1.6_idf_v5.5.4`** — esp-matter's own
recommended ESP-IDF version, for reproducible builds. **Do not bump this to an
ESP-IDF v6.0.x image**: esp-matter does not support ESP-IDF v6.0 yet (confirmed
via the esp-matter repo, which still recommends v5.5.4, and Espressif's own
v6.0 announcement, which lists Matter as not yet available on that branch).
Revisit this once esp-matter publishes a v6.0-based release tag.

Open the environment:
```bash
./tools/dev.sh          # wraps: docker run --rm -it -v "$PWD":/project -w /project espressif/esp-matter:release-v1.6_idf_v5.5.4 /bin/bash
```

Default build target: classic **ESP32 (WROOM-32)**. Also supports esp32c3 / c6 /
s3 / h2. (C6/H2 additionally support Thread; classic ESP32 is Wi-Fi.)

## Common commands

Build (inside the container). The image's entrypoint already sources both
export.sh scripts before you get a shell, so that step isn't needed — but the
entrypoint also leaves you in `$ESP_MATTER_PATH` (`/opt/espressif/esp-matter`),
**not** `/project`, no matter what `-w` you pass to `docker run`. Use the
absolute container path, not a path relative to wherever your shell happens
to have landed:
```bash
cd /project/firmware/light
idf.py set-target esp32
idf.py build
```

Two build-system gotchas already worked around in this repo's CMakeLists.txt
files, worth knowing if you copy `firmware/light/` for a new device type:
- Don't add `$ESP_MATTER_PATH/examples/common` to `EXTRA_COMPONENT_DIRS`
  unless you actually use something from it (e.g. `app_reset`) — ESP-IDF
  tries to build every component it finds there, and `app_reset` needs a
  `button` component this project doesn't declare.
- `-DCHIP_HAVE_CONFIG_H` (and a few other flags) must be set **project-wide**
  via `idf_build_set_property(...)` before `project(...)` in the root
  `CMakeLists.txt` — connectedhomeip/esp_matter compile as their own
  component, so setting it only on `main` isn't enough.

Flash (from the HOST, not the container — Docker Desktop on macOS can't see USB):
```bash
esptool.py -p <PORT> write_flash \
  0x0     firmware/light/build/bootloader/bootloader.bin \
  0x8000  firmware/light/build/partition_table/partition-table.bin \
  0x20000 firmware/light/build/matter_light.bin
```

Generate factory partition + QR code (offline, inside the container):
```bash
pip install esp-matter-mfg-tool
./tools/gen_factory.sh                 # writes out/<vid_pid>/<uuid>/*-qrcode.png + *-partition.bin
```
Then flash the factory partition to the `fctry` slot at `0x3E0000`.

## Repository layout

```
firmware/light/          On/Off light — the reference device
  main/app_main.cpp       plain esp-matter code; LED on GPIO 2 (WROOM-32)
  partitions.csv          OTA A/B slots + separate fctry partition (fits 4 MB)
  sdkconfig.defaults      factory-data provider + custom partition table
firmware/switch/          On/Off switch — second device type, copied from light/
  main/app_main.cpp       button on GPIO 4 (WROOM-32) sends a real OnOff
                           Toggle command to bound device(s) via esp-matter's
                           client invoke API (client::cluster_update() +
                           client::interaction::invoke::send_request());
                           reference wiring is a breadboard pushbutton
                           (GND -> button -> GPIO), deliberately not the
                           onboard BOOT/PROG button; requires a controller
                           (e.g. Home Assistant) to set up a Binding-cluster
                           entry to an actual target device first
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
tools/
  dev.sh                  opens the Docker dev environment
  gen_factory.sh          local QR + factory partition generator
  product-wizard/         local no-build web UI that walks through picking a
                           device type + module + GPIO and generates the
                           build + flash commands to run yourself (see its
                           own README)
docs/getting-started.md   step-by-step first-device guide
SECURITY.md               flash encryption / secure boot / signed OTA guidance
```

## Conventions

- Certificates: use Matter **test** certs + test VID (0xFFF1–0xFFF4) for hobby
  use. A real certified product needs a CSA-issued Vendor ID.
- Adding a new device type: copy `firmware/light/` to e.g. `firmware/switch/` and
  swap the endpoint type in `app_main.cpp` (esp-matter offers `on_off_light_switch`,
  `dimmable_light`, `temperature_sensor`, `contact_sensor`, etc.).
- No CI yet — a `.github/workflows/build.yml` was tried (build both device
  types x every target, attach `.bin`s to the release on a `v*` tag) but the
  Docker image is multi-GB and the pull/cache step stalled out on GitHub's
  runners; reverted rather than leave a flaky workflow in place. Releases
  are manual for now: build + flash per `docs/getting-started.md`.
- Image tag is pinned to `release-v1.6_idf_v5.5.4` in `tools/dev.sh` for
  reproducible builds — see "Development environment" above for why not
  `latest` or an IDF v6.0.x tag.

## Never commit

- `out/`, `*.bin`, `*-qrcode.png`, generated CSVs — per-device secrets.
- `*.pem` / secure-boot signing keys — keep offline and backed up.
(These are already covered by `.gitignore`.)

## Open next steps (discussed, not yet done)

1. Add a third device type (temperature or contact sensor) — `firmware/light/`
   and `firmware/switch/` are both there now as duplication examples.
2. ~~Make the switch actually control a bound device~~ — done. Button presses
   on `firmware/switch/` now send a real `OnOff::Toggle` via
   `client::cluster_update()` / `client::interaction::invoke::send_request()`,
   verified against esp-matter's own `examples/light_switch/main/app_driver.cpp`
   and confirmed on real hardware (GPIO4 breadboard button, ESP32 WROOM-32):
   the old `Failed to get attribute handle` error is gone (that was caused by
   `on_off_light_switch`'s OnOff cluster being CLIENT-only — no local server
   attribute exists to update, see `esp_matter_endpoint.cpp`). Without an
   actual Binding-cluster entry, a press now logs `esp_matter_client: failed
   to notify the bound cluster changed` instead, which is expected: it means
   the command path works but no bound peer is configured yet. Binding a
   switch to a real target device (e.g. `firmware/light/`) needs a
   controller with a Bindings UI — Home Assistant has one, Apple/Google Home
   don't — and hasn't been tested end-to-end yet (still open: does a press
   actually toggle a bound light once bindings are configured?).
3. Implement Matter **OTA** so devices update themselves over the air from a
   GitHub Release `.bin` (start from USB flashing, add signed OTA on top).
4. Revisit CI: same build recipe as before, but pull the image once outside
   the matrix (e.g. a setup job, or a self-hosted/larger runner) instead of
   per-job, so it doesn't stall out again.
5. Move to an ESP-IDF v6.0.x-based image once esp-matter publishes one
   (update `tools/dev.sh` and the wizard's `ESP_MATTER_IMAGE` together).
6. ~~`firmware/switch/`'s onboard BOOT/PROG button (GPIO 0) was unreliable~~ —
   fixed. Switching to an external breadboard pushbutton on GPIO 4
   (GND -> button -> GPIO, confirmed not a boot-strapping pin) resolved it:
   multiple clean presses tested reliably on real hardware, confirming
   GPIO 0's dual role as boot-mode-select (not the debounce/ISR logic) was
   the actual culprit.

## Note on hardware/USB

Building happens in Docker; flashing happens on the host with `esptool`. On Linux
you can alternatively pass the device into the container
(`--device=/dev/ttyUSB0`) and use `idf.py flash`, but the host route works on
macOS/Windows/Linux alike.
