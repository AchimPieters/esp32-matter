# esp32-matter

ESP32 **Matter** devices, built in a fully transparent way — no hidden code, no
cloud, no data sharing. Development happens in the **Docker-based ESP-IDF
environment** from the StudioPieters guide, adapted for Matter.

Everything is built on the open-source [esp-matter](https://github.com/espressif/esp-matter)
SDK. You compile inside Docker, flash from your host, and generate the
commissioning QR code locally. Matter itself is local-first: pairing runs over
Bluetooth + your LAN and control runs over your local network. Nothing leaves
your home unless you deliberately add a cloud hub. With Home Assistant it stays
entirely local.

Default target is the classic **ESP32 (WROOM-32)** to match the StudioPieters
setup; the code also runs on ESP32-C3/C6/S3/H2.

> Dev environment reference: <https://www.studiopieters.nl/esp32-homekit-development/>

---

## What's in here

```
esp32-matter/
├── firmware/
│   ├── light/               # A minimal On/Off light — your starting point
│   │   ├── main/app_main.cpp
│   │   ├── main/CMakeLists.txt
│   │   ├── CMakeLists.txt
│   │   ├── partitions.csv   # OTA A/B slots + separate factory partition
│   │   └── sdkconfig.defaults
│   └── switch/              # A minimal On/Off switch — copied from light/
│       └── (same layout as light/)
├── tools/
│   ├── dev.sh               # Opens the Docker dev environment
│   ├── gen_factory.sh       # Generates the factory partition + QR code locally
│   └── product-wizard/      # Local no-build web UI to set up a device + generate build/flash commands
├── docs/
│   └── getting-started.md  # Step-by-step first-device guide
└── SECURITY.md             # How to enable flash encryption + secure boot
```

## Quick start (Docker)

1. **Install Docker Desktop** and make sure it's running.

2. **Pull the esp-matter image** (ESP-IDF + esp-matter pre-installed). Pinned
   to esp-matter's own recommended ESP-IDF version (v5.5.4) for reproducible
   builds — esp-matter doesn't support ESP-IDF v6.0.x yet:
   ```bash
   docker pull espressif/esp-matter:release-v1.6_idf_v5.5.4
   ```

3. **Clone this repository:**
   ```bash
   git clone https://github.com/AchimPieters/esp32-matter.git
   cd esp32-matter
   ```
   (No `--recursive` — esp-matter/connectedhomeip live inside the Docker
   image, not as git submodules here.)

4. **Open the dev environment** (mounts the repo at /project inside the container):
   ```bash
   ./tools/dev.sh
   ```
   Or manually, exactly in the StudioPieters style:
   ```bash
   docker run --rm -it -v "$PWD":/project -w /project espressif/esp-matter:release-v1.6_idf_v5.5.4 /bin/bash
   ```

5. **Build inside the container.** The image's entrypoint already activates
   ESP-IDF and esp-matter for you, but it also leaves the shell in
   `$ESP_MATTER_PATH`, not `/project` — use the absolute path:
   ```bash
   cd /project/firmware/light
   idf.py set-target esp32        # or esp32c3 / esp32c6 / esp32s3 / esp32h2
   idf.py build
   ```

6. **Flash from your host** (Docker Desktop on macOS can't see the USB port, so
   flash outside the container — install esptool with `pip3 install esptool`).
   `0x1000` is the bootloader offset for classic ESP32 specifically — it's
   `0x0` on every later chip (C3/C6/S3/H2). `ota_data_initial.bin` is
   required too: this partition table has no "factory" app slot, so the
   bootloader needs it to know which OTA slot to boot.
   ```bash
   esptool.py -p <PORT> write_flash \
       0x1000  firmware/light/build/bootloader/bootloader.bin \
       0x8000  firmware/light/build/partition_table/partition-table.bin \
       0x10000 firmware/light/build/ota_data_initial.bin \
       0x20000 firmware/light/build/matter_light.bin
   ```

7. **Generate your QR code (offline)** and **commission** — see
   `docs/getting-started.md`.

## Adding more device types

`firmware/switch/` is a second example, copied from `firmware/light/` with the
endpoint type swapped to `on_off_light_switch` — its button sends a real OnOff
Toggle command to whatever it's bound to, via esp-matter's client invoke API.

`firmware/contact-sensor/` is a third example (`contact_sensor` endpoint
type): a digital input (e.g. a door/window reed switch) reported through the
Boolean State cluster's StateValue attribute. Note for anyone copying it as a
template for another sensor-style device: updating that attribute from app
code needs esp-matter's cluster-specific setter API
(`BooleanStateCluster::SetStateValue()`), not the generic `attribute::update()`
that `firmware/light/` uses — BooleanState is implemented via a newer
"code-driven" cluster class in this SDK version, and the generic attribute
store returns `ESP_ERR_NOT_SUPPORTED` for it. See the comment above
`update_contact_state()` in its `app_main.cpp` for the full explanation and
the working pattern.

`firmware/outlet/` (`on_off_plug_in_unit` endpoint type) is a fourth
example: a physical button that toggles its *own* Matter OnOff attribute
directly (`attribute::update()`, same server-side pattern as
`firmware/light/`), so — unlike `firmware/switch/` — it shows up as a
real, controllable tile in Apple/Google Home. Apple Home labels it
"Outlet"/"Stopcontact" rather than "Switch"; that's expected, not a bug —
Matter's device type library has no separate device type for "a wall
switch with its own on/off state" distinct from a plug-in outlet (every
device type with "Switch" in the name is a client/input device, none of
them a controllable output). See the header comment in its `app_main.cpp`
for the full explanation.

To add another type, copy any of the four folders and swap the endpoint
type in `app_main.cpp` — esp-matter provides ready-made types like
`dimmable_light`, `temperature_sensor`, and many more.

## Updates

No CI yet (see CLAUDE.md — an automated build/release workflow was tried
but the multi-GB Docker image stalled out on GitHub-hosted runners, so it
was reverted rather than left flaky). Build + flash a new version yourself
following the Quick Start steps above whenever you change `app_main.cpp`.

All three device types also ship with Matter's **OTA Requestor** cluster
enabled (`CONFIG_ENABLE_OTA_REQUESTOR=y`), so once a device is bound to an
OTA Provider node on the same fabric, it can fetch and install updates over
the air using the existing `ota_0`/`ota_1` A/B partition slots — no app
code needed, esp-matter wires the requestor up automatically. Setting up
that binding needs a controller (same idea as `firmware/switch`'s Binding
cluster for sending commands) and an OTA Provider node actually serving a
`.bin` — this repo doesn't run one yet. The requestor side has been
verified on real hardware (boots cleanly, registers the cluster, no
errors); the provider side and a full transfer are open, tracked in
CLAUDE.md's next steps alongside the binding test, which hits the same
"needs a second commissioned device + tooling" wall.

## Honest expectations

This is a genuine firmware project, not a no-code app. You work from the command
line with ESP-IDF and C++ (inside Docker, so no local toolchain mess). That's the
price of "no hidden code, nothing shared" — in return you own and can read every
part of it.

## License

MIT — see `LICENSE`.
