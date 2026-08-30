# esp32-matter

**A fully transparent toolkit for building [Matter](https://csa-iot.org/all-solutions/matter/)
smart-home devices on ESP32 chips.** No hidden code, no cloud, no telemetry.
Everything is plain, readable C++ built on the open-source
[esp-matter](https://github.com/espressif/esp-matter) SDK — you compile it
inside Docker, flash it from your own machine, and generate the commissioning
QR code offline.

Matter is local-first by design: pairing runs over Bluetooth LE + your Wi-Fi/
Thread network, and control runs over your LAN. Nothing leaves your home unless
*you* add a cloud hub. Paired with Home Assistant it stays 100 % local.

- **69 device types** already implemented — lights, sensors, switches, locks,
  thermostats, appliances, energy meters, bridges and more (full catalog below).
- **A local Product Wizard** (`tools/product-wizard/`) that walks you through
  picking a device type, an ESP32 module and GPIO pins, then hands you the exact
  build + flash commands to run — no guesswork.
- **Reproducible builds** — pinned to esp-matter's own recommended
  `espressif/esp-matter:release-v1.6_idf_v5.5.4` Docker image.
- **License: MIT.**

> Development-environment reference (Docker + ESP-IDF, StudioPieters style):
> <https://www.studiopieters.nl/esp32-homekit-development/> — by AchimPieters /
> [StudioPieters](https://www.studiopieters.nl).

---

## Table of contents

- [Is this for me?](#is-this-for-me)
- [Core principles](#core-principles)
- [What you need](#what-you-need)
- [Path A — the easy way: the Product Wizard](#path-a--the-easy-way-the-product-wizard)
- [Path B — the manual way: build your first device by hand](#path-b--the-manual-way-build-your-first-device-by-hand)
- [Commissioning & controller apps](#commissioning--controller-apps)
- [Quick-power-cycle factory reset](#quick-power-cycle-factory-reset)
- [Over-the-air (OTA) updates](#over-the-air-ota-updates)
- [The device catalog (69 types)](#the-device-catalog-69-types)
- [For advanced users](#for-advanced-users)
  - [Repository layout](#repository-layout)
  - [Anatomy of a firmware folder](#anatomy-of-a-firmware-folder)
  - [Adding a new device type](#adding-a-new-device-type)
  - [Build-system gotchas](#build-system-gotchas)
  - [Supported chips & targets](#supported-chips--targets)
  - [Two-chip device types](#two-chip-device-types)
- [Security hardening](#security-hardening)
- [Hardware-verification status](#hardware-verification-status)
- [Honest expectations](#honest-expectations)
- [Project status & contributing](#project-status--contributing)
- [License & credits](#license--credits)

---

## Is this for me?

| You want to… | Start here |
|---|---|
| Build one specific device with the least fuss | [Path A — the Product Wizard](#path-a--the-easy-way-the-product-wizard) |
| Understand every step, or you're comfortable with a terminal | [Path B — build by hand](#path-b--the-manual-way-build-your-first-device-by-hand) |
| Add a brand-new device type or hack on the firmware | [For advanced users](#for-advanced-users) |
| Just see what's possible | [The device catalog](#the-device-catalog-69-types) |

**This is a real firmware project, not a no-code app.** Even with the wizard you
will paste two commands into a terminal and flash a chip over USB. In return you
own and can read every line of what runs on your device.

---

## Core principles

These are non-negotiable for anything that lands in this repo:

1. **No hidden code.** Everything is plain, readable C++/shell/YAML on top of the
   open-source esp-matter SDK. No prebuilt or obfuscated framework blobs.
2. **No cloud, no data sharing, no telemetry.** Matter is local-first: pairing
   over BLE + LAN, control over the local network. Nothing phones home.
3. **Local QR generation.** Commissioning data and the QR code are generated
   entirely offline, on your machine.
4. **Reproducible.** The toolchain is pinned to one Docker image tag.
5. **MIT-licensed.**

---

## What you need

**Hardware**

- An **ESP32 board**. The classic **ESP32 DevKit (WROOM-32)** is the default
  target and matches every wiring example. Also supported: ESP32-C2 / C3 / C5 /
  C6 / C61 / S3 / H2 (see [Supported chips](#supported-chips--targets)).
- A **USB cable** (and a USB-to-serial adapter if your board has no on-board
  USB).
- Whatever the device needs — an LED, a relay module, a sensor, etc. Each
  firmware's `main/app_main.cpp` header comment documents its wiring.

**Software (host machine only)**

- **[Docker Desktop](https://www.docker.com/products/docker-desktop/)**,
  installed and running. All compiling happens inside a container — no local
  ESP-IDF toolchain to install or break.
- **Python 3 + esptool** for flashing over USB:
  ```bash
  python3 -m pip install esptool
  esptool.py --version
  ```
- A **Matter controller** to commission the device: Apple Home, Google Home,
  Amazon Alexa, SmartThings, or **Home Assistant** (recommended — see
  [Commissioning](#commissioning--controller-apps)).

**First-time setup (once):**

```bash
git clone https://github.com/AchimPieters/esp32-matter.git
cd esp32-matter
docker pull espressif/esp-matter:release-v1.6_idf_v5.5.4
```

No `--recursive` — esp-matter and connectedhomeip live inside the Docker image,
not as git submodules here.

---

## Path A — the easy way: the Product Wizard

A local, offline, no-build web page that generates the exact commands for your
device. Nothing is uploaded anywhere; it stores your product list in your
browser's `localStorage` only.

1. **Open it:**
   ```bash
   open tools/product-wizard/index.html
   ```
   (or double-click the file, or drag it into a browser tab).

2. **Create a product**, give it a name, then walk the steps:
   - **Get Started** — pick one of the 65 device types (grouped into Lighting,
     Switches & Remotes, Outlets & Power, Sensors, Climate & Ventilation,
     Doors/Windows & Closures, Appliances, Doorbell & Chime, Bridges).
   - **Select Module** — pick your ESP32 chip.
   - **Configure Device** — set the GPIO pins, pick a sensor/driver chip where
     the device type offers a choice, tick any optional features.
   - **Customise & Review** — check the summary.
   - **Generate Firmware** — copy the two generated commands.

3. **Run the first command** (builds inside Docker *and* generates your factory
   partition + QR code):
   ```bash
   docker run --rm -it -v "<path-to-esp32-matter>:/project" -w /project \
     espressif/esp-matter:release-v1.6_idf_v5.5.4 bash -c \
     "cd /project/firmware/<type> && <sed edits> && idf.py set-target <chip> && idf.py build && \
      PRODUCT_NAME='<name>' /project/tools/gen_factory.sh"
   ```

4. **Run the second command** on your host (flashes everything over USB —
   replace `<PORT>`):
   ```bash
   esptool.py --chip <chip> -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash \
     --flash_mode dio --flash_size 4MB --flash_freq 40m \
     <offset> firmware/<type>/build/bootloader/bootloader.bin \
     0x8000   firmware/<type>/build/partition_table/partition-table.bin \
     0x10000  firmware/<type>/build/ota_data_initial.bin \
     0x20000  firmware/<type>/build/matter_<type>.bin \
     0x3E0000 firmware/<type>/out/*/*/*-partition.bin
   ```

5. **Scan the QR code** (`firmware/<type>/out/**/**-qrcode.png`) in your
   controller app.

The wizard's own `tools/product-wizard/README.md` documents every step, every
option and every device type in detail.

---

## Path B — the manual way: build your first device by hand

This builds `firmware/light/` — a minimal On/Off light with an LED on **GPIO 2**
of a classic ESP32 (WROOM-32). It's the reference every other device type is
copied from.

### 1. Open the dev environment

```bash
./tools/dev.sh
```

This is a thin wrapper around the StudioPieters-style command:

```bash
docker run --rm -it -v "$PWD":/project -w /project \
  espressif/esp-matter:release-v1.6_idf_v5.5.4 /bin/bash
```

You're now inside the container with ESP-IDF **and** esp-matter already
activated (the image's entrypoint sources both `export.sh` scripts for you).

> ⚠️ **The entrypoint drops you in `$ESP_MATTER_PATH`
> (`/opt/espressif/esp-matter`), *not* `/project`** — regardless of the `-w`
> flag. Your repo is still mounted at `/project`; always use the absolute path.

### 2. Build (inside the container)

```bash
cd /project/firmware/light
idf.py set-target esp32          # or esp32c2 / esp32c3 / esp32c5 / esp32c6 / esp32c61 / esp32s3 / esp32h2
idf.py build
```

Build output lands in `firmware/light/build/`, visible on your host too via the
mounted volume.

### 3. Generate your commissioning QR code (offline, inside the container)

```bash
pip install esp-matter-mfg-tool          # once; chip-cert already ships in the image
cd /project/firmware/light
/project/tools/gen_factory.sh
```

This writes, under `firmware/light/out/<vid_pid>/<uuid>/`:

- `*-partition.bin` — the factory partition (per-device certificates +
  commissioning data)
- `*-qrcode.png` — the QR code to scan
- a manual pairing code (also in the generated CSV)

Re-run `gen_factory.sh` **once per physical unit** — every device needs its own
identity.

### 4. Flash from your host (outside the container)

Docker Desktop on macOS/Windows can't see the USB serial port, so flash from the
host with esptool. Find `<PORT>` with `ls /dev/tty.*` (macOS) or
`ls /dev/ttyUSB*` (Linux).

```bash
esptool.py --chip esp32 -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000   firmware/light/build/bootloader/bootloader.bin \
  0x8000   firmware/light/build/partition_table/partition-table.bin \
  0x10000  firmware/light/build/ota_data_initial.bin \
  0x20000  firmware/light/build/matter_light.bin \
  0x3E0000 firmware/light/out/*/*/*-partition.bin
```

Notes:

- **Bootloader offset is `0x1000` on the classic ESP32**, but `0x0` on every
  later chip (C2/C3/C5/C6/C61/S3/H2). `idf.py build` prints the authoritative
  offsets for whatever you just built — cross-check there if a board won't boot.
- **`ota_data_initial.bin` at `0x10000` is required.** This partition table has
  no "factory" app slot (only OTA A/B slots), so the bootloader needs the OTA
  data partition to know which slot to boot.
- **`0x3E0000` is the `fctry` partition** — where the factory data / QR
  identity from step 3 goes.

On Linux you can alternatively pass the port into the container
(`docker run … --device=/dev/ttyUSB0 …`) and use `idf.py flash` directly.

### 5. Commission

Open your controller app → *Add device / Add Matter device* → scan the QR code
from step 3. Toggle it and watch the LED (and the serial log) respond.

---

## Commissioning & controller apps

| Controller | Works with this repo's TEST certificates? |
|---|---|
| **Home Assistant** | ✅ Yes — commissions self-signed test devices without complaint. Recommended, and fully local. |
| **chip-tool** (CLI) | ✅ Yes. |
| **Apple Home** | ⚠️ Usually **no** in the normal consumer flow — it validates the device-attestation chain against Apple's bundled list of CSA-recognised roots, and a locally generated test PAA will never be on it. |
| **Google Home** | ⚠️ Same limitation as Apple Home. |

`tools/gen_factory.sh` generates a self-signed **test** Product Attestation
Authority the first time it runs and reuses it after. For **home / hobby use**
with Home Assistant this is all you need. For a **real certified product** you
need your own Vendor ID from the [CSA](https://csa-iot.org/) and a matching
attestation chain — see `SECURITY.md` and the comment at the top of
`tools/gen_factory.sh`.

Default identity uses a **test Vendor ID** (`0xFFF1`–`0xFFF4`). Override via
environment variables to `gen_factory.sh` (`VENDOR_ID=…`, `PRODUCT_ID=…`, …).

---

## Quick-power-cycle factory reset

Every one of the single-chip device types supports a no-button factory reset,
matching how real plug-in / hard-wired smart-home gear (which often has no
accessible button once installed) does it:

> **Power the device off and on 3 times in a row** — roughly 2 seconds on,
> 2 seconds off, repeated 3 times — and it factory-resets and re-enters
> commissioning setup mode.

A plain counter in its own `boot_info` NVS namespace increments on every boot and
starts a 10-second one-shot timer; if the device stays powered past that, the
counter clears (a "confirmed" normal boot, so a single power outage never
triggers it). Three quick reboots call esp-matter's own
`esp_matter::factory_reset()` after the Matter server has started. Build-verified
across every device type; not yet exhaustively hardware-tested.

---

## Over-the-air (OTA) updates

- **OTA Requestor cluster: enabled.** 67 of the 69 firmware types build with
  `CONFIG_ENABLE_OTA_REQUESTOR=y`, which adds Matter's OTA Requestor cluster to
  the root node automatically (no app code). The `ota_0` / `ota_1` A/B slots and
  `otadata` partition it needs are already in every `partitions.csv`. Verified on
  real hardware for `firmware/contact-sensor/` and `firmware/switch/` (boots
  clean, cluster registered). The two exceptions are `firmware/ble-mesh-bridge/`
  and `firmware/zigbee-bridge/`, which keep Espressif's own unmodified reference
  `sdkconfig.defaults`.
- **A real OTA transfer is not wired up yet.** It needs an OTA Provider node
  commissioned onto the same fabric, actually serving a `.bin`. Downloading an
  update straight from a GitHub Release also needs a small bridge piece that
  doesn't exist here yet (Matter OTA is BDX-from-a-Provider, not arbitrary
  URLs). Tracked in `CLAUDE.md` → *Open next steps*.

For now: rebuild and re-flash following [Path A](#path-a--the-easy-way-the-product-wizard)
or [Path B](#path-b--the-manual-way-build-your-first-device-by-hand) whenever you
change a firmware. Bump `PROJECT_VER` in the firmware's root `CMakeLists.txt` per
release.

---

## The device catalog (69 types)

Every folder under `firmware/` is real, buildable firmware — not a placeholder.
Each `main/app_main.cpp` opens with a long header comment documenting exactly
what was checked against the Matter spec / chip datasheets, the wiring, and any
known limitations. `CLAUDE.md` has an exhaustive per-device engineering log.

**Legend:** ✅ = verified end-to-end on real hardware · 🔨 = build-verified in
Docker only (most of the catalog — hardware for every device type simply wasn't
on hand). Where a device type offers a choice of sensor/driver chip, only some
of those chips are hardware-verified; see the firmware's own header comment.

<details open>
<summary><b>Lighting</b> (7)</summary>

| Device | Folder | Notes |
|---|---|---|
| On/Off Light | `light/` | ✅ The reference device. LED on GPIO 2. |
| Dimmable Light | `dimmable-light/` | ✅ OnOff + LevelControl, real PWM via LEDC. Remembers brightness across off/on. |
| Color Light (RGB / RGBW / RGBWW) | `color-light/` | 🔨 Extended Color Light, Hue/Saturation (+ ColorTemperature in RGBWW/RGBCCT mode). 3 build-time hardware variants. |
| Addressable LED Strip | `addressable-light/` | ✅ (WS2812B, SK6812 RGBW) Single-color over 8 chips: WS2812B/WS2813/WS2815/SK6812/SK6812-RGBW/WS2805 (RMT), APA102 (SPI), SM2335EGH (bit-banged). |
| Color Temperature Light | `color-temperature-light/` | 🔨 Tunable white — cool + warm channels only, no RGB. |
| Mounted On/Off Control | `mounted-onoff-control/` | 🔨 In-wall relay module (Shelly 1 / Sonoff Basic class). |
| Mounted Dimmable Load Control | `mounted-dimmable-load-control/` | 🔨 In-wall dimmer module. |

</details>

<details>
<summary><b>Switches & Remotes</b> (client / control-only devices)</summary>

| Device | Folder | Notes |
|---|---|---|
| On/Off Light Switch | `switch/` | ✅ (single button) 1–4 independent buttons, each its own bindable endpoint. |
| Generic Switch | `generic-switch/` | 🔨 "Smart button" — single/double/triple/long-press events for automations. |
| Dimmer Switch | `dimmer-switch/` | 🔨 Button + rotary encoder, sends On/Off + LevelControl to a bound light. |
| Color Dimmer Switch | `color-dimmer-switch/` | 🔨 Dimmer Switch + a second encoder for hue. |
| Mode Select | `mode-select/` | 🔨 Physical scene/activity selector (Home / Away / Night). |
| Door Lock Controller | `door-lock-controller/` | 🔨 Lock / Unlock buttons for a bound door lock (timed invoke). |
| Thermostat Controller | `thermostat-controller/` | 🔨 Rotary-knob setpoint remote. |
| Window Covering Controller | `window-covering-controller/` | 🔨 Open / Close / Stop remote. |
| Closure Controller | `closure-controller/` | 🔨 Open / Close remote for a garage door / shutter. |
| Pump Controller | `pump-controller/` | 🔨 On/Off remote for a bound pump. |
| Control Bridge | `control-bridge/` | 🔨 Universal light remote (On/Off + Level + Color). |
| On/Off Sensor | `on-off-sensor/` | 🔨 A PIR/radar module that sends On/Off directly to a bound light — no sensing cluster of its own. |

</details>

<details>
<summary><b>Outlets & Power</b></summary>

| Device | Folder | Notes |
|---|---|---|
| Outlet (On/Off Plug-in Unit) | `outlet/` | ✅ Button toggles its own OnOff. Optional relay, status LED, and one of 6 power-monitor chips (2nd Electrical Sensor endpoint). |
| Dimmable Plug-In Unit | `dimmable-plug/` | 🔨 Outlet + LevelControl (PWM into a dimmer module). |
| Electrical Meter | `electrical-meter/` | 🔨 Whole-circuit energy meter, 6-chip power-monitor subsystem. |
| Electrical Utility Meter | `electrical-utility-meter/` | 🔨 Electrical Meter + Meter Identification + root-node time sync. |
| Meter Reference Point | `meter-reference-point/` | 🔨 The simplest firmware here — Identify + root time sync only. |
| Battery Storage | `battery-storage/` | 🔨 Home battery system — PowerSource battery metrics + composed Electrical Sensor. |
| Solar Power | `solar-power/` | 🔨 Solar inverter power meter (exported-energy framing). |
| Energy EVSE | `evse/` | 🔨 EV charger controller. ⚠️ Gates an *already-certified* EVSE's enable input — does **not** implement the SAE J1772 Control Pilot. |

</details>

<details>
<summary><b>Sensors</b></summary>

| Device | Folder | Notes |
|---|---|---|
| Contact Sensor | `contact-sensor/` | ✅ Reed switch → Boolean State. |
| Temperature Sensor | `temperature-sensor/` | ✅ (SHT3x, DHT11, DHT22) Choice of 7 chips: SHT3x/SHT4x/AHT20/DHT11/DHT22/DS18B20/BME280. Multi-endpoint (temp + humidity). |
| Humidity Sensor | `humidity-sensor/` | 🔨 Standalone humidity, 6-chip choice (no DS18B20). |
| Light Sensor | `light-sensor/` | 🔨 LDR (analog/ADC) or BH1750 (I2C). |
| Occupancy Sensor | `occupancy-sensor/` | ✅ (PIR) PIR / RCWL-0516 / HLK-LD2410. |
| Air Quality Sensor | `air-quality-sensor/` | 🔨 CCS811 eCO2/eTVOC + optional MQ-7/MQ-131/PMS5003/ZE08-CH2O/MiCS-4514 + temp/humidity. |
| Pressure Sensor | `pressure-sensor/` | 🔨 BMP280 barometric. |
| Flow Sensor | `flow-sensor/` | 🔨 YF-S201-class Hall-effect pulse flow sensor. |
| Soil Sensor | `soil-sensor/` | 🔨 Capacitive soil-moisture probe + optional DS18B20 soil temp. |
| Water Leak Detector | `water-leak-detector/` | 🔨 LM393 water probe → Boolean State (`true` = leak). |
| Water Freeze Detector | `water-freeze-detector/` | 🔨 DS18B20 + threshold classifier. |
| Rain Sensor | `rain-sensor/` | 🔨 Same LM393 board as the leak detector, mounted for rain. |
| Smoke CO Alarm | `smoke-co-alarm/` | 🔨 MQ-2 smoke + MQ-7 CO + optional temp/humidity. Life-safety alarm cluster. |

</details>

<details>
<summary><b>Climate & Ventilation</b></summary>

| Device | Folder | Notes |
|---|---|---|
| Thermostat (Heat + Cool) | `thermostat/` | 🔨 Real control loop. Relay / bound relay / native **OpenTherm** master. Optional rotary encoder + GC9A01/ST7789/SSD1306 display. |
| Room Air Conditioner | `room-air-conditioner/` | 🔨 Cool-only Thermostat + Fan + optional filter monitoring + temp/humidity. |
| Heat Pump | `heat-pump/` | 🔨 Composed: root + Thermostat child endpoint. Compressor + reversing-valve relays. Optional 6-chip power monitor. |
| Fan | `fan/` | 🔨 PWM speed (0–100 %) via LEDC. |
| Air Purifier | `air-purifier/` | 🔨 Fan + HEPA/carbon filter monitoring (time-based life estimate). |
| Extractor Hood | `extractor-hood/` | 🔨 Fan + grease/carbon filter monitoring. |

</details>

<details>
<summary><b>Doors, Windows & Closures</b></summary>

| Device | Folder | Notes |
|---|---|---|
| Door Lock | `door-lock/` | 🔨 Servo (thumb-turn retrofit) or relay (strike). Optional position sensor. |
| Window Covering | `window-covering/` | 🔨 Two relays (up/down), time-based position estimate. |
| Closure | `closure/` | 🔨 Garage door / roller shutter / awning (Closure Control cluster). |
| Water Valve | `valve/` | 🔨 Relay solenoid, cluster-owned auto-close countdown. |

</details>

<details>
<summary><b>Appliances</b></summary>

| Device | Folder | Notes |
|---|---|---|
| Refrigerator | `refrigerator/` | 🔨 Composed: root + Fridge + Freezer child cabinets. |
| Dishwasher | `dishwasher/` | 🔨 Operational State cycle + mode + alarm + door interlock. |
| Laundry Washer | `laundry-washer/` | 🔨 Wash cycle, spin-speed / rinse-count controls. |
| Laundry Dryer | `laundry-dryer/` | 🔨 Heat + tumble, dryness-level → cycle time. |
| Oven | `oven/` | 🔨 Composed: root + heated cavity. Oven Mode + Oven Cavity Operational State (hand-assembled clusters). |
| Microwave Oven | `microwave-oven/` | 🔨 Duty-cycle power control. ⚠️ Gates an OEM cooking module's enable input, not a consumer microwave's internals. |
| Cooktop | `cooktop/` | 🔨 Composed: Cooktop + Cook Surface. TemperatureLevel power steps; optional MAX6675/MAX31855 thermocouple. |
| Water Heater | `water-heater/` | 🔨 Thermostat + Water Heater Management (Boost) + DS18B20. Optional 6-chip power monitor. |
| Robotic Vacuum Cleaner | `robot-vacuum/` | 🔨 Run/Clean modes + Operational State. Two drive motors; no navigation (honest scope cut). |
| Pump | `pump/` | 🔨 On/Off + speed + OperationMode. Optional temp/pressure/flow measurement. |
| Irrigation System | `irrigation-system/` | 🔨 Single-zone valve + flow measurement + timed cycle. |
| Temperature Controlled Cabinet | `temperature-controlled-cabinet/` | 🔨 Standalone wine cooler / mini-fridge. |

</details>

<details>
<summary><b>Doorbell & Chime</b></summary>

| Device | Folder | Notes |
|---|---|---|
| Doorbell | `doorbell/` | 🔨 Button → Chime client. Binds to a Chime device. |
| Chime | `chime/` | 🔨 Piezo-buzzer receiver — two tone patterns, LEDC audio-frequency PWM. |

</details>

<details>
<summary><b>Bridges & infrastructure</b></summary>

| Device | Folder | Wizard? | Notes |
|---|---|---|---|
| RF433 / IR Bridge | `rf-ir-bridge/` | ✅ | Learns EV1527/PT2262 433 MHz codes **and** NEC IR button presses → dynamic Generic Switch endpoints. Both protocols independently toggleable. |
| BLE Mesh Bridge | `ble-mesh-bridge/` | ✖ | Verbatim port of esp-matter's reference. Exposes provisioned BLE Mesh nodes as Matter endpoints. |
| Zigbee Bridge | `zigbee-bridge/` | ✖ | Two-chip (ESP32-S3 host + ESP32-H2 RCP). Verbatim port of esp-matter's reference. |
| Thread Border Router | `thread-border-router/` | ✖ | Two-chip (ESP32-S3 + ESP32-H2). Real OpenThread Border Router — makes a separate Thread mesh reachable. |
| Matter Camera | `camera/` | ✖ | Two-chip (ESP32-P4 + ESP32-C6) split architecture. Verbatim port of esp-matter's reference; needs the external Amazon KVS WebRTC SDK. See `firmware/camera/README.md`. |

</details>

---

## For advanced users

### Repository layout

```
esp32-matter/
├── firmware/<type>/          One folder per device type (69). Each has:
│   ├── main/app_main.cpp       plain esp-matter C++ + a long header comment
│   ├── main/CMakeLists.txt
│   ├── CMakeLists.txt          root project file; sets PROJECT_VER + project-wide flags
│   ├── partitions.csv          OTA A/B slots + separate fctry partition (fits 4 MB)
│   └── sdkconfig.defaults      factory-data provider, custom partitions, OTA requestor
├── tools/
│   ├── dev.sh                  opens the pinned Docker dev environment
│   ├── gen_factory.sh          offline factory partition + QR generator
│   └── product-wizard/         local no-build web UI (index.html + README.md)
├── modules/                    per-chip SVG illustrations used by the wizard
├── Icons/                      hand-drawn device-type icon source (SVG)
├── docs/getting-started.md     first-device walkthrough
├── SECURITY.md                 flash encryption / secure boot / signed OTA
├── CLAUDE.md                   exhaustive engineering log — read this for depth
└── LICENSE                     MIT
```

**`CLAUDE.md` is the real design document.** It carries a full per-device
engineering log: what was checked against the Matter spec and chip datasheets,
every SDK gotcha found, what's hardware-verified vs. build-verified, and why each
scoping decision was made.

### Anatomy of a firmware folder

Every device type is self-contained and follows the same shape as
`firmware/light/`:

- **`main/app_main.cpp`** — creates the Matter node + endpoint(s), wires cluster
  callbacks to GPIO, runs the app. The header comment is the per-device spec.
- **Compile-time configuration is via `#define`s** near the top of
  `app_main.cpp` (e.g. `SENSOR_TYPE`, `OUTLET_POWER_MONITOR`,
  `SWITCH_BUTTON_COUNT`, `*_GPIO`). The wizard edits these with `sed`; you can
  edit them by hand.
- **`partitions.csv`** — identical layout everywhere: `nvs`, `nvs_keys`,
  `otadata`, `phy_init`, `ota_0` (`0x20000`), `ota_1` (`0x200000`), `fctry`
  (`0x3E0000`). No factory app slot — OTA data drives boot slot selection.
- **`sdkconfig.defaults`** — factory-data provider reading from `fctry`, the
  custom partition table, `CONFIG_ENABLE_OTA_REQUESTOR=y`, BLE + IPv6 for
  commissioning, `CONFIG_MBEDTLS_HKDF_C=y`. Security hardening options are
  present but commented out (see `SECURITY.md`).

### Adding a new device type

1. Copy the closest existing folder (`cp -r firmware/light firmware/my-thing`).
2. In `main/app_main.cpp`, swap the endpoint type — esp-matter ships ready-made
   helpers (`endpoint::dimmable_light::create()`,
   `endpoint::temperature_sensor::create()`,
   `endpoint::robotic_vacuum_cleaner::create()`, …). Prefer a **complete
   top-level helper** where one exists: it creates the endpoint's Descriptor
   cluster automatically via `common::create<T>()`. Hand-assembling from
   `endpoint::create()` + individual `cluster::*::create()` calls is only needed
   when the helper doesn't wire up a cluster you need — and then you must call
   `cluster::descriptor::create()` yourself first (a missing Descriptor cluster
   is silently tolerated by Home Assistant but rejected by Apple Home).
3. In the root `CMakeLists.txt`, rename `project(matter_light)` to
   `project(matter_my_thing)` — the binary name follows.
4. Verify against the **CSA device type XML** (inside the Docker image at
   `$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/data_model/1.6/device_types/`)
   which clusters are `mandatoryConform` vs. `optionalConform` — don't trust a
   secondary summary.
5. Some cluster attributes are **not** writable via the generic
   `attribute::update()` — "code-driven" cluster classes (BooleanState,
   TemperatureMeasurement, OccupancySensing, …) need their own setter looked up
   through the data model provider's registry. If there's a folder for the
   cluster under `data_model_provider/clusters/`, it's code-driven.
6. Add it to the wizard: a new `DEVICE_TYPES` entry + `COMPONENT_LIBRARY`
   entries in `tools/product-wizard/index.html`. See its README for the
   mechanisms (`componentOptions`, `extraPickers`, `clusterOptions`,
   `numberFields`, …).

### Build-system gotchas

Already worked around in this repo's `CMakeLists.txt` files, worth knowing if
you copy one:

- **Don't add `$ESP_MATTER_PATH/examples/common` to `EXTRA_COMPONENT_DIRS`**
  unless you use something from it — ESP-IDF tries to build every component it
  finds there, and `app_reset` needs a `button` component this project doesn't
  declare.
- **`-DCHIP_HAVE_CONFIG_H` (and a few other flags) must be set project-wide**
  via `idf_build_set_property(...)` *before* `project(...)` in the root
  `CMakeLists.txt` — connectedhomeip/esp_matter compile as their own component,
  so setting it only on `main` isn't enough.

### Supported chips & targets

Default target is the classic **ESP32 (WROOM-32)**. Also builds for `esp32c2`,
`esp32c3`, `esp32c5`, `esp32c6`, `esp32c61`, `esp32s3`, `esp32h2` via
`idf.py set-target`.

- **Wi-Fi-only:** classic ESP32, C2, C3, C61, S3.
- **802.15.4 / Thread-capable:** C5, C6, H2 (this repo builds Matter-over-Thread
  for them, never Zigbee firmware).
- **Excluded:** ESP32-H4 / H21 (radio defines commented out in ESP-IDF v5.5.4's
  `soc_caps.h`), ESP32-P4 (no radio of its own), ESP32-S2 (no BLE, so no
  standard Matter commissioning).
- **Do not bump the Docker image to an ESP-IDF v6.0.x tag** — esp-matter doesn't
  support it yet.

### Two-chip device types

`camera/`, `zigbee-bridge/` and `thread-border-router/` are genuinely two-chip
architectures (a host chip + a radio co-processor / media chip). They keep
Espressif's own unmodified reference build files, are **not** in the wizard, and
their READMEs explain the extra build steps. `camera/` additionally needs an
external SDK you clone separately.

---

## Security hardening

For real deployments, see **`SECURITY.md`** for the full walkthrough of:

- **Flash encryption** + **Secure Boot v2** (the commented-out
  `CONFIG_SECURE_*` options in every `sdkconfig.defaults`).
- **Signed OTA** (`esp_matter_ota_requestor_encrypted_init()`).
- **`nvs_keys` partition** is already declared `encrypted` in `partitions.csv`.

Leave these **off while learning** — they make re-flashing harder — then turn
them on before shipping anything.

**Never commit:** `out/`, `*.bin`, `*-qrcode.png`, generated CSVs (per-device
secrets), `*.pem` / signing keys. All already covered by `.gitignore`.

---

## Hardware-verification status

Honesty matters here. Of the 69 device types:

- **Verified end-to-end on real hardware** (built, flashed, commissioned,
  exercised): `light`, `contact-sensor`, `switch` (single-button config),
  `outlet`, `temperature-sensor` (SHT3x / DHT11 / DHT22), `light-sensor` (LDR
  boot only), `dimmable-light`, `addressable-light` (WS2812B, SK6812 RGBW),
  `occupancy-sensor` (PIR).
- **Everything else is build-verified in Docker only** — the hardware for each
  device type simply wasn't on hand. The firmware compiles cleanly for its
  target(s); the runtime wiring is written carefully against the SDK source and
  chip datasheets, and flagged as unverified in both the code and the wizard.

Each firmware's `app_main.cpp` header comment and `CLAUDE.md` state precisely
what has and hasn't been confirmed.

---

## Honest expectations

This is a genuine firmware project, not a no-code app. You work from the command
line with ESP-IDF and C++ (inside Docker, so no local toolchain mess). That's the
price of "no hidden code, nothing shared" — in return you own and can read every
part of it.

Getting a device onto **Apple Home / Google Home** in their normal consumer flow
needs a real CSA Vendor ID and attestation chain this repo can't provide.
**Home Assistant works today, fully local.**

---

## Project status & contributing

- **No CI.** An automated build/release workflow was tried, but the multi-GB
  Docker image stalled GitHub-hosted runners, so it was reverted rather than
  left flaky. Releases are manual: build + flash per this README.
- **Issues and PRs welcome.** New device types should follow the existing
  pattern (a self-contained `firmware/<type>/` folder, a documented
  `app_main.cpp` header, verification against the CSA device type XML, a wizard
  entry) and note clearly what is and isn't hardware-tested.
- Read **`CLAUDE.md`** before a substantial change — it captures the reasoning
  behind decisions that aren't obvious from the code alone.

---

## License & credits

**MIT** — see [`LICENSE`](LICENSE).

Built by **Achim Pieters / [StudioPieters®](https://www.studiopieters.nl)** on
top of Espressif's open-source [esp-matter](https://github.com/espressif/esp-matter)
and [ESP-IDF](https://github.com/espressif/esp-idf), and the
[Connectivity Standards Alliance](https://csa-iot.org/)'s Matter specification.
Development environment adapted from the
[StudioPieters ESP32 HomeKit development guide](https://www.studiopieters.nl/esp32-homekit-development/).
