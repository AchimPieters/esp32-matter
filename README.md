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
setup; the code also runs on ESP32-C2/C3/C5/C6/C61/S3/H2.

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
   idf.py set-target esp32        # or esp32c2 / esp32c3 / esp32c5 / esp32c6 / esp32c61 / esp32s3 / esp32h2
   idf.py build
   ```

6. **Flash from your host** (Docker Desktop on macOS can't see the USB port, so
   flash outside the container — install esptool with `pip3 install esptool`).
   `0x1000` is the bootloader offset for classic ESP32 specifically — it's
   `0x0` on every later chip (C2/C3/C5/C6/C61/S3/H2). `ota_data_initial.bin` is
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
endpoint type swapped to `on_off_light_switch` — each button sends a real
OnOff Toggle command to whatever it's bound to, via esp-matter's client
invoke API. Supports 1-4 independent buttons (`#define SWITCH_BUTTON_COUNT`,
default 1), each its own endpoint independently bindable to a different
target device — the same way Matter models a physical multi-gang wall
switch. `client::set_request_callback()` only needs registering once
regardless of button count (it's endpoint-agnostic by design). Both the
single-button default and the 4-button path are build-verified in Docker;
only the single-button configuration has been tested on real hardware so
far. See its `app_main.cpp` header comment for the full explanation,
including a documented limitation around two buttons pressed at almost the
same instant (the second Toggle waits for the first press to fully release).

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

`OUTLET_OUTPUT_TYPE` defaults to a relay module (active-LOW, matching what
an actual power outlet/smart plug normally switches with; always check
your specific module's own wiring, since polarity isn't universal) — not a
wizard-exposed choice, since offering a plain LED as an equally-weighted
alternative was misleading rather than useful; LED (active-HIGH) is still
there in the source for breadboard testing, just edit the `#define`
directly. A separate, independently-optional `OUTLET_STATUS_LED_GPIO` (off
by default) adds a third LED some real plug hardware has: a small
indicator that continuously mirrors on/off state, wired to its own GPIO —
different from the required Identify LED, which only blinks temporarily on
a controller's Identify command. And `OUTLET_POWER_MONITOR` optionally
compiles in one of **six** power-monitoring chips — **BL0942** and
**CSE7766** (UART), **BL0937**, **HLW8012**, and **CSE7759** (GPIO
pulse-frequency), and **ADE7953** (I2C) — feeding a second Matter
endpoint (Electrical Sensor, device type 0x0510). Power readings use a
hand-written push-style `Delegate` (adapted from esp-matter's own
`examples/all_device_types_app` reference, since the generic cluster
config for it is an undocumented raw pointer); energy readings use
esp-matter's own ready-made `ElectricalEnergyMeasurement` API directly —
two different integration patterns for two clusters in the same file.
Every chip's protocol/formula was checked against its own manufacturer
datasheet, which caught two real bugs during development (BL0942 had
current/voltage at swapped byte offsets; CSE7766's status byte was
mischaracterized) — see the header comment in `app_main.cpp` for the
full per-chip story, including which ones (CSE7759, ADE7953) could only
be partially or indirectly verified. Build-verified in Docker for all 7
power-monitor configurations; not hardware-tested (no module of any of
the six chips was physically available here).

`firmware/temperature-sensor/` is a fifth example, and this repo's first
sensor device with more than one supported chip: change `SENSOR_TYPE` in
`app_main.cpp` (or let the wizard's sed command do it) to pick from **SHT3x,
SHT4x, AHT20, DHT11, DHT22, DS18B20, or BME280** — whichever you actually
have wired up. Each is a genuinely different protocol/command set (I2C,
single-wire bit-banged, or 1-Wire), not just a different pin, so all seven
drivers live behind `#if SENSOR_TYPE == ...` in the one file rather than
being a runtime setting. SHT3x, DHT11, and DHT22 are verified on real
hardware in this repo; the other four are implemented from their
datasheets/reference drivers but not personally hardware-tested here —
each driver's own comment in `app_main.cpp` says which and why. It's also
this repo's first multi-endpoint device: temperature (`temperature_sensor`
device type) and humidity (`humidity_sensor`) each get their own endpoint,
since Matter has no single device type combining both (DS18B20 is
temperature-only, so it skips the humidity endpoint entirely). Both
`TemperatureMeasurementCluster` and `RelativeHumidityMeasurementCluster`
are the same kind of "code-driven" cluster class as `firmware/contact-
sensor/`'s BooleanState — updating them needs `SetMeasuredValue()`, not
the generic `attribute::update()`. See its `app_main.cpp` header comment
for the full explanation, each sensor's wiring, and why classic ESP32
needs an external sensor at all (it has no internal temperature sensor
peripheral, unlike later chips such as S2/S3/C3/C6).

`firmware/light-sensor/` is a sixth example, and this repo's second
device with a choice of sensor chip (after temperature): change
`SENSOR_TYPE` in `app_main.cpp` (or let the wizard's sed command do it)
to pick between an **LDR/photoresistor** (this repo's only analog/ADC
device — every other type is digital: GPIO, I2C, single-wire, or 1-Wire)
and a **BH1750** digital ambient light sensor over I2C (almost always
sold as a "GY-30"/"GY-302" breakout). The LDR forms a voltage divider
read via ESP-IDF's `esp_adc/adc_oneshot.h`, converted through ADC
calibration (`esp_adc/adc_cali.h` — classic ESP32 only supports the
"line fitting" scheme, not "curve fitting"; the code uses ESP-IDF's own
documented `#if`/`#elif` portable pattern so the same source still
builds correctly on chips that only have the other one) into millivolts,
then into lux via the standard photoresistor characteristic curve
(`R_LDR = R10 * (10/lux)^gamma`, using the common GL5528's typical
datasheet values). BH1750 reports lux directly over I2C — no
voltage-divider math needed — using its documented "One Time
H-Resolution Mode" command (`0x20`) and `lux = raw / 1.2` conversion.
Either way, Matter's Illuminance Measurement cluster stores
`MeasuredValue` logarithmically (`10000 * log10(lux) + 1`, the same
encoding Zigbee's ZCL illuminance cluster uses), not raw lux.
`IlluminanceMeasurementCluster` is the same kind of "code-driven" cluster
class as the temperature sensor's clusters — same `SetMeasuredValue()`
fix needed. Unlike every other device type here, neither sensor has been
tested against physical hardware in this repo (none was on hand when it
was written) — implemented carefully from datasheet sources and flagged
as such (in the code, and in the wizard), same standard as the
temperature sensor's unverified sensor chips. See its `app_main.cpp`
header comment for the full explanation and wiring for both sensors.

`firmware/dimmable-light/` is a seventh example, and this repo's first
device type with a real actuator beyond plain on/off — every prior type
is either a digital GPIO output, a sensor, or (for the switch) a
remote-control client with no output of its own. Uses esp-matter's
`dimmable_light` endpoint (OnOff + LevelControl, vs. `firmware/light/`'s
OnOff-only `on_off_light`), driving the LED as real PWM via ESP-IDF's
`driver/ledc.h` — one LEDC timer + channel, `LEDC_LOW_SPEED_MODE` for
portability across every module this repo targets — instead of a plain
`gpio_set_level()`. LevelControl's `CurrentLevel` turned out to be a
plain "ember" attribute like OnOff, not one of the newer "code-driven"
cluster classes other sensors in this repo needed a special setter for —
confirmed by checking esp-matter's own `data_model_provider/clusters/`
has no `level_control/` folder — so it uses the exact same
`attribute::PRE_UPDATE` reaction pattern `firmware/light/` already uses
for OnOff, just for a second cluster too. `CurrentLevel`'s 1-254 range
maps directly onto LEDC's 0-255 duty range with no remapping math needed.
The light boots Off (matching every other device type here), and output
is on/off state × level together — turning off doesn't forget the
brightness, so turning back on restores it, same as a real dimmer. See
its `app_main.cpp` header comment for the full explanation, including
exactly what was checked against esp-matter's own SDK/reference example
before writing any of it. Validated end to end on real hardware (ESP32
WROOM-32, LED on GPIO 2 — this device type's own default, no `#define`
edits needed): built and flashed via the wizard's own generated commands,
commissioned into Home Assistant with a clean PASE/CASE handshake, then
both On/Off and the brightness slider exercised live from Home
Assistant's UI — confirmed via the serial log, not just the controller's
own display.

`firmware/window-covering/` is an eighth example, and this repo's first
device type with continuous, multi-second physical movement rather than
an instant response. Uses esp-matter's `window_covering` endpoint (Lift +
PositionAwareLift features — up/down travel with percentage position
reporting, no Tilt). Unlike every other cluster this repo uses,
WindowCovering doesn't drive hardware or simulate movement by itself —
confirmed directly in esp-matter's source: it only validates commands and
calls an app-supplied `Delegate`'s `HandleMovement()`/`HandleStopMotion()`.
This file's delegate drives two relay outputs (UP/DOWN, active-LOW,
mutually exclusive by construction) via a shared FreeRTOS task, estimating
position through linear interpolation against a calibrated full-travel
time — no position sensor assumed, the same technique ESPHome's/Tasmota's
own time-based cover components use. Position accuracy is therefore only
as good as that calibration; a stalled, slipped, or hand-moved covering
drifts out of sync until the next full open/close command re-anchors it.
Cross-checked against connectedhomeip's own real reference delegate
(`examples/chef/common/clusters/window-covering/`) for the correct
attribute update pattern before writing any of it — see its `app_main.cpp`
header comment for the full explanation. Build-verified in Docker; not
hardware-tested (no motor/relay hardware for this device type physically
available when written).

To add another type, copy any of the eight folders and swap the endpoint
type in `app_main.cpp` — esp-matter provides ready-made types like
`extended_color_light`, `occupancy_sensor`, and many more.

## Updates

No CI yet (see CLAUDE.md — an automated build/release workflow was tried
but the multi-GB Docker image stalled out on GitHub-hosted runners, so it
was reverted rather than left flaky). Build + flash a new version yourself
following the Quick Start steps above whenever you change `app_main.cpp`.

All eight device types also ship with Matter's **OTA Requestor** cluster
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
