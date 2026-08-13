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
firmware/contact-sensor/  Contact sensor — third device type, copied from switch/
  main/app_main.cpp       reed switch on GPIO 4 (WROOM-32) reports open/closed
                           via the Boolean State cluster's StateValue
                           attribute; reacts to both edges (ANYEDGE, not
                           NEGEDGE) since it tracks live state, not discrete
                           presses. StateValue can't be written through
                           esp-matter's generic attribute::update() in this
                           SDK version — BooleanState is a newer "code-driven"
                           cluster class, not the generic ember attribute
                           store — so it goes through
                           BooleanStateCluster::SetStateValue() instead,
                           looked up via the data model provider's registry
                           (see the comment above update_contact_state() in
                           app_main.cpp for the full story; this bug class is
                           worth checking for in any other "server cluster
                           attribute app code needs to write" case going
                           forward — OnOff was a plain ember attribute and
                           didn't hit it, BooleanState did)
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

1. ~~Add a third device type~~ — done: `firmware/contact-sensor/` (a reed-
   switch-style Boolean State sensor), verified on real hardware. Building
   it surfaced a new class of bug worth remembering for any future sensor
   type: esp-matter's generic `attribute::update()` can't write every
   server attribute in this SDK version — clusters implemented via the
   newer "code-driven" cluster classes (BooleanState is one) need their
   own setter API instead (`BooleanStateCluster::SetStateValue()` here),
   looked up through the data model provider's registry. See the
   repository layout entry above and the comment above
   `update_contact_state()` in `firmware/contact-sensor/main/app_main.cpp`.
   Since then, taken through the *entire* product-wizard flow for real
   (product "Front Door Sensor"): the wizard's own generated Docker
   command (sed + build + `gen_factory.sh`) and flash command, run
   verbatim against the real repo, then the resulting QR code scanned and
   commissioned via Apple Home — full PASE/CASE handshake, fabric add,
   WiFi join, `Commissioning complete — device is now paired`, zero
   errors. First device type besides the light to reach that bar; the
   wizard's "validated end to end on real hardware" claim is now true for
   both, not just light. A temperature sensor (analog/ADC-driven, unlike
   the three GPIO-digital types that exist now) is the natural next type
   to add.
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
   the command path works but no bound peer is configured yet. The button's
   local GPIO2 Identify LED now also doubles as an on/off indicator (flips
   on every confirmed press), independent of any binding — stress-tested
   with ~30 rapid taps, no missed/double presses.

   Also since taken through the wizard's own commissioning path for real
   (product "Living Room Switch", after `esptool erase_flash` — reflashing
   over a previously-commissioned board leaves stale fabric data in NVS,
   since write-flash never touches that partition, and the device comes
   up already "Operational" instead of freshly commissionable if you
   skip that). Commissioning itself succeeds cleanly
   (`Commissioning complete — device is now paired`, no errors), but
   Apple Home then shows it as a generic "Matter Accessory" / "Niet
   geschikt" tile with a house icon — expected, not a bug: there's no
   server attribute for Apple Home to display (CLIENT-only OnOff, see
   above) and no Bindings UI to configure the one thing this device type
   actually needs.

   Still open: an actual end-to-end binding test (does a press really
   toggle a bound light?) needs two devices commissioned onto the same
   fabric plus a controller with a Bindings UI (Home Assistant has one,
   Apple/Google Home don't). Only one physical board is available right
   now, so the plan was to commission a Linux-simulated `chip-lighting-app`
   (buildable from source already present in the esp-matter Docker image;
   not prebuilt) as the second "device". That's blocked on something more
   fundamental, discovered while scoping it: initial Matter commissioning
   needs BLE, and **Docker Desktop on macOS has no Bluetooth passthrough**
   into containers — the same class of limitation already documented above
   for USB. `chip-tool` (prebuilt in the esp-matter image at
   `/usr/local/bin/chip-tool`) is therefore unusable for commissioning from
   inside that image on this host; it would need a native macOS build of
   connectedhomeip (Xcode CLT + Python + GN/ninja, a multi-hour first
   build), which was deferred rather than done speculatively. Revisit when
   either a second physical board + a controller that already handles BLE
   (e.g. the phone that did the original Apple Home pairing, if it grows
   binding support) becomes available, or when a native macOS chip-tool
   build is worth the investment.
3. Implement Matter **OTA** — partially done. All three firmware types now
   ship `CONFIG_ENABLE_OTA_REQUESTOR=y`, which adds the OTA Requestor
   cluster to the root node endpoint entirely via Kconfig — esp-matter's
   own core startup (`esp_matter_core.cpp`) calls
   `esp_matter_ota_requestor_init()`/`_start()` automatically once that
   flag is on, so no app code was needed. Confirmed on real hardware for
   `firmware/contact-sensor/` and `firmware/switch/` (clean boot, cluster
   registered, zero errors); `firmware/light/` builds identically but
   wasn't separately reflash-tested since the code path is generic to
   every device type, not device-specific.

   Still open, and blocked on the same wall as the binding test above: a
   real OTA **transfer** needs an OTA Provider node commissioned onto the
   same fabric, actually serving a `.bin` (e.g. `chip-ota-provider-app`,
   source-only in the esp-matter image, not prebuilt) — which needs
   `chip-tool`-class commissioning tooling, which needs BLE, which Docker
   Desktop on macOS doesn't pass through. The original goal of updating
   "from a GitHub Release `.bin`" also needs a small bridge piece that
   doesn't exist yet: something that downloads the release asset and feeds
   it to whatever OTA Provider is running (Matter OTA doesn't fetch
   arbitrary URLs directly — only BDX from a Provider on the fabric).
   Revisit alongside the binding test once native macOS `chip-tool` (or
   equivalent commissioning tooling) exists. Signed/encrypted OTA
   (`esp_matter_ota_requestor_encrypted_init()`, see
   `examples/light/main/app_main.cpp` in the SDK) is a further step after
   that.
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
