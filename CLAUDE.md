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

Default build target: classic **ESP32 (WROOM-32)**. Also supports esp32c2 / c3 /
c5 / c6 / c61 / s3 / h2. (C5/C6/H2 additionally support Thread — their
802.15.4 radio can physically run Zigbee too, but this repo never builds
Zigbee firmware for it, only Matter-over-Thread; classic ESP32/C2/C3/C61/S3
are Wi-Fi-only. ESP32-H4 and ESP32-H21 exist in the SDK but are excluded
here — their radio capability defines are commented out in ESP-IDF's own
`soc_caps.h` on this repo's pinned v5.5.4, so they're not reliably usable
on this exact SDK version. ESP32-P4 is excluded too — no Wi-Fi/BLE/802.15.4
radio of its own, needs an external companion chip. ESP32-S2 is excluded —
has Wi-Fi but no BLE, so it can't do Matter's standard commissioning flow.)

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
  main/app_main.cpp       1-4 independent buttons (#define SWITCH_BUTTON_
                           COUNT, default 1), each its own on_off_light_
                           switch endpoint — the same way a physical multi-
                           gang wall switch is modelled in Matter: one node,
                           one endpoint per gang, each independently
                           bindable to a different target device. Button 1
                           is GPIO 4 (WROOM-32, this repo's original single-
                           button default); buttons 2-4 default to GPIO
                           16/17/18 (none of them strapping pins). Every
                           button sends a real OnOff Toggle command to
                           *its own* bound device(s) via esp-matter's client
                           invoke API (client::cluster_update() +
                           client::interaction::invoke::send_request());
                           client::set_request_callback() is registered
                           once, globally, not per endpoint — it's endpoint-
                           agnostic by design, so one registration correctly
                           serves every button. One shared FreeRTOS task +
                           debounce queue handles all configured buttons
                           (the original single-button logic, reused
                           as-is) — known limitation: two buttons pressed
                           at almost the same instant serialize (the second
                           Toggle sends only after the first press is fully
                           handled and released), acceptable for how these
                           are actually used. With more than one button the
                           Identify LED stops doubling as a local on/off
                           indicator (see firmware/switch/'s own header
                           comment for why: no single "switch state" is
                           left for one shared LED to represent once
                           buttons can be bound to different targets).
                           Reference wiring per button is a breadboard
                           pushbutton (GND -> button -> GPIO), deliberately
                           not the onboard BOOT/PROG button; requires a
                           controller (e.g. Home Assistant) to set up a
                           Binding-cluster entry per endpoint to an actual
                           target device first. SWITCH_BUTTON_COUNT=1
                           (regression) and =4 both build-verified in
                           Docker; only the original single-button/GPIO 4
                           configuration has been tested on real hardware
                           so far — multi-button is new and flagged as
                           such in the wizard.
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
firmware/outlet/         On/Off Plug-in Unit — fourth device type, combines
                           light/'s server-attribute pattern with switch/'s
                           button/debounce pattern
  main/app_main.cpp       button on GPIO 4 (WROOM-32) toggles its own Matter
                           OnOff attribute directly (attribute::update(),
                           same call as firmware/light/) driving an output
                           on GPIO 2 — unlike firmware/switch/, this shows up
                           as a real controllable tile in Apple/Google Home
                           (labeled "Outlet"/"Stopcontact", not "Switch" —
                           expected: Matter's device type library has no
                           separate device type for a wall switch's own
                           on/off state distinct from a plug-in outlet, see
                           the header comment in app_main.cpp for the full
                           explanation, checked directly against the spec's
                           device_types/ folder). #define OUTLET_OUTPUT_TYPE
                           selects RELAY (default, active-LOW — common for
                           low-cost opto-isolated relay modules, documented
                           as "always check your specific module" since
                           polarity isn't universal — a relay is what a
                           real power outlet/smart plug actually switches
                           with) or LED (active-HIGH, mainly for breadboard
                           testing without a relay on hand; the only choice
                           that existed before OUTLET_OUTPUT_TYPE did, and
                           still this option's default until real-world
                           feedback pointed out relay is the realistic
                           default for actual outlet hardware). A separate
                           #define OUTLET_STATUS_LED_GPIO optionally adds a
                           third, independent LED — some real plug hardware
                           has its own small indicator LED, on its own
                           GPIO, that continuously mirrors on/off state,
                           distinct from the Identify LED (which only
                           blinks temporarily on an Identify command and
                           says nothing about on/off state). Off by default
                           (GPIO_NUM_NC, ESP-IDF's "not connected"
                           sentinel — checked at runtime in set_output(),
                           not via #if, since GPIO_NUM_NC is a gpio_num_t
                           enumerator, not a preprocessor macro).
                           #define OUTLET_POWER_MONITOR optionally compiles
                           in one of 6 power-monitoring chip drivers —
                           BL0942/CSE7766 (UART), BL0937/HLW8012/CSE7759
                           (GPIO pulse-frequency), ADE7953 (I2C) — feeding a
                           second Matter endpoint (Electrical Sensor device
                           type, 0x0510) built from two different esp-matter
                           integration patterns: ElectricalPowerMeasurement
                           via a hand-written push-style Delegate subclass
                           (adapted from esp-matter's own
                           examples/all_device_types_app reference, since
                           esp-matter's generic cluster::create() config for
                           it is an undocumented void* — not risked) and
                           ElectricalEnergyMeasurement via esp-matter's own
                           ready-made free-function API
                           (data_model_provider/clusters/
                           electrical_energy_measurement/integration.h —
                           no custom Delegate needed there). All 6 chips'
                           protocols were checked against their own
                           manufacturer datasheets, not secondary sources,
                           per instruction — this caught two real bugs:
                           BL0942's response packet had current and voltage
                           byte offsets swapped from an earlier
                           secondary-source draft, and CSE7766's Adj status
                           byte was mischaracterized as "measurement valid"
                           when the datasheet defines it as "cycle
                           complete/incomplete" (same resulting skip logic,
                           different actual meaning — comment corrected).
                           BL0937 and HLW8012's existing formulas were
                           independently re-derived from their datasheets
                           and confirmed already correct. CSE7759 (assumed
                           to share HLW8012's chip family per a secondary
                           source only — its own datasheet wasn't
                           obtainable) and ADE7953 (least-certain of the
                           six — only partially confirmed against Analog
                           Devices' own datasheet across several attempts)
                           are flagged as such in the code, the wizard's
                           Configure Device step, and its Generate Firmware
                           step. See the file's header comment for full
                           per-chip protocol/formula detail and exact
                           sourcing. Every power-monitor chip's own GPIOs
                           (BL0942/CSE7766's UART pins, BL0937/HLW8012/
                           CSE7759's shared SEL/CF/CF1 pulse pins, ADE7953's
                           I2C pins) are wizard-configurable fields in
                           Configure Device once that chip is picked, not
                           just hardcoded #defines — real, reported gap:
                           the wizard originally let you pick a chip but
                           never showed its pins anywhere. Build-verified in
                           Docker for all 7 power-monitor configurations
                           (none + 6 chips) x both output types, plus the
                           status LED both off (default) and on — not
                           hardware-tested (no module of any of the 6 chips,
                           or a separate status LED, was physically
                           available).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/temperature-sensor/  Temperature + humidity sensor — fifth device
                           type, first over I2C/single-wire/1-Wire (not
                           plain GPIO) and first multi-endpoint device
  main/app_main.cpp       one #define SENSOR_TYPE selects which of 7
                           sensor drivers compiles in — SHT3x, SHT4x,
                           AHT20, DHT11, DHT22, DS18B20, BME280 (list
                           chosen as what's actually most common
                           beginner-through-pro, checked against current
                           sources — an earlier draft only had the
                           sensors this repo's own testing happened to
                           have on hand, which nearly missed BME280
                           despite it being the community's most-
                           recommended all-rounder). SHT3x/DHT11/DHT22
                           verified on real hardware; SHT4x/AHT20/
                           DS18B20/BME280 implemented from datasheet/
                           reference driver, not personally hardware-
                           tested here (documented as such per-driver,
                           and surfaced in the wizard's Configure Device
                           step too). SENSOR_PIN_1/SENSOR_PIN_2 are
                           generic on purpose (I2C SDA/SCL or single-wire
                           DATA, unused-but-harmless second pin for the
                           single-wire sensors) so the wizard doesn't
                           need per-sensor field variants. Classic ESP32
                           has no internal temperature sensor peripheral
                           (arrived with later chips: S2/S3/C3/C6), hence
                           external sensors at all. Exposes temperature
                           (temperature_sensor device type, endpoint 1)
                           and humidity (humidity_sensor, endpoint 2) as
                           two endpoints on one node — skips the humidity
                           endpoint for DS18B20, which is temperature-
                           only — since Matter has no single device type
                           combining both from one sensor chip.
                           TemperatureMeasurement and
                           RelativeHumidityMeasurement are the same kind
                           of "code-driven" cluster class as
                           firmware/contact-sensor/'s BooleanState — same
                           fix needed (SetMeasuredValue() via the data
                           model provider's registry, not the generic
                           attribute::update()), confirmed the same way by
                           reading esp_matter_data_model.cpp's set_val().
                           BME280's compensation math is Bosch's own
                           reference fixed-point algorithm, reproduced
                           verbatim (fetched from their public
                           BME280_driver repo) rather than approximated —
                           this sensor's raw ADC counts are meaningless
                           without applying it against per-chip
                           calibration coefficients read from its NVM.
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/light-sensor/    Ambient light sensor — sixth device type, and
                           this repo's second sensor with a choice of
                           chip (after temperature-sensor): one
                           #define SENSOR_TYPE selects LIGHT_SENSOR_LDR
                           (this repo's only analog/ADC driver — every
                           other type/sensor here is digital: GPIO, I2C,
                           single-wire, or 1-Wire) or LIGHT_SENSOR_BH1750
                           (digital ambient light sensor over I2C, almost
                           always sold as a "GY-30"/"GY-302" breakout)
  main/app_main.cpp       LDR/photoresistor voltage divider on GPIO 34
                           by default (ADC1 channel 6, WROOM-32 —
                           deliberately ADC1, not ADC2, since ADC2 is
                           unreliable once Wi-Fi is active, which this
                           device needs to be commissioned at all). Reads
                           via ESP-IDF's driver/adc_oneshot.h, calibrated
                           via esp_adc/adc_cali.h using ESP-IDF's own
                           documented #if ADC_CALI_SCHEME_CURVE_FITTING_
                           SUPPORTED / #elif ..._LINE_FITTING_SUPPORTED
                           portable pattern (classic ESP32 only has line
                           fitting; the pattern still builds correctly on
                           chips that only have curve fitting instead).
                           Converts millivolts -> lux via the standard
                           photoresistor characteristic curve
                           (R_LDR = R10 * (10/lux)^gamma, GL5528 typical
                           datasheet values as the reference). BH1750 is
                           read via driver/i2c_master.h (same new-style
                           I2C API as firmware/temperature-sensor/'s I2C
                           sensors) using its documented "One Time
                           H-Resolution Mode" command (0x20, 1 lx
                           resolution, sensor self-powers-down after each
                           reading — Power On (0x01) is resent before
                           every measurement) and returns lux directly
                           via the sensor's own raw/1.2 conversion — no
                           voltage-divider math needed, unlike the LDR.
                           Both then feed lux into Matter's logarithmic
                           MeasuredValue encoding (10000 * log10(lux) + 1
                           — same encoding Zigbee's ZCL illuminance
                           cluster uses). IlluminanceMeasurementCluster is
                           the same kind of "code-driven" cluster class as
                           the temperature sensor's clusters, same
                           SetMeasuredValue() fix needed. Unlike every
                           other device type here, neither sensor is
                           hardware-verified in this repo (none on hand
                           when written) — both build-verified in Docker
                           for both SENSOR_TYPE values, flagged as
                           unverified in the code and the wizard (via the
                           same per-component COMPONENT_LIBRARY `verified`
                           flag the temperature sensor uses, not a
                           separate whole-device-type flag — see the
                           wizard entry below).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/dimmable-light/  Dimmable light — seventh device type, and this
                           repo's first with a real actuator beyond plain
                           on/off (every prior type is a digital GPIO
                           output, a sensor, or a remote-control switch)
  main/app_main.cpp       `dimmable_light` endpoint (OnOff + LevelControl
                           clusters, vs. firmware/light/'s OnOff-only
                           `on_off_light`) — checked directly against
                           esp-matter's own endpoint::dimmable_light::add()
                           in esp_matter_endpoint.cpp for the exact cluster
                           composition rather than assumed. Output is real
                           PWM via ESP-IDF's driver/ledc.h (the LEDC
                           peripheral: one timer + one channel,
                           LEDC_LOW_SPEED_MODE for portability across every
                           module this repo targets, LEDC_AUTO_CLK,
                           8-bit/0-255 duty resolution), not a plain
                           gpio_set_level() — DIMMABLE_LIGHT_LED_GPIO must
                           be an LEDC-capable pin (true for nearly every
                           GPIO except input-only ones). LevelControl's
                           CurrentLevel confirmed to be a plain ember
                           attribute, not a "code-driven" cluster class
                           (checked by confirming esp-matter's
                           data_model_provider/clusters/ has no
                           level_control/ folder, unlike BooleanState/
                           TemperatureMeasurement/ElectricalPowerMeasurement
                           elsewhere in this repo) — so it uses the exact
                           same attribute::PRE_UPDATE + attribute::update()
                           pattern as OnOff, confirmed against esp-matter's
                           own examples/light/app_driver.cpp. CurrentLevel's
                           1-254 range is used directly as the LEDC duty
                           value (0-255) with no remapping math — close
                           enough that the ~0.4% difference at full
                           brightness isn't perceptible, and simpler than
                           the official example's separate 0-100% remap
                           (only needed there because it goes through a
                           generic led_driver component this repo
                           deliberately doesn't depend on). Output = on/off
                           state x level together, same as a real dimmer:
                           turning off doesn't forget the brightness.
                           Boots Off (on_off_lighting.start_up_on_off left
                           at its config default of 0), matching every
                           other device type's boot-to-known-state
                           convention in this repo, rather than
                           esp-matter's own example (which restores
                           whatever state preceded a power loss). Also
                           calls attribute::set_deferred_persistence() on
                           CurrentLevel (same call the official example
                           makes, for the same reason: avoids flash wear
                           from writing NVS on every step of a brightness
                           slider drag). Build-verified in Docker and
                           validated end to end on real hardware (ESP32
                           WROOM-32, LED on GPIO 2): flashed via the
                           wizard's own generated commands, commissioned
                           into Home Assistant (full PASE/CASE handshake,
                           zero errors — confirmed independently via the
                           serial log, not just the controller's own UI),
                           then both On/Off and the brightness slider
                           exercised live from Home Assistant — every
                           CurrentLevel change during a slider drag
                           correctly reached set_output() and updated the
                           LEDC duty, confirmed by the serial log's
                           "Light level set to N/254" line for each step.
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/window-covering/  Roller shade / curtain — eighth device type, and
                           this repo's first with continuous, multi-second
                           physical movement instead of an instant on/off/
                           dim response
  main/app_main.cpp       `window_covering` endpoint (Identify + Groups +
                           WindowCovering clusters, Lift + PositionAwareLift
                           features only — no Tilt). Confirmed directly in
                           esp-matter's own source that, unlike every prior
                           device type, this cluster does NOT drive hardware
                           or simulate movement on its own — it only
                           validates commands, stores TargetPosition, and
                           calls an app-supplied Delegate's HandleMovement()/
                           HandleStopMotion(). Cross-checked against
                           connectedhomeip's own real reference delegate
                           (examples/chef/common/clusters/window-covering/)
                           for the correct Attribute Get/Set +
                           MatterReportingAttributeChangeCallback() pattern
                           — chef's own version does an instant jump with no
                           timed movement (headless/simulated, no real
                           motor), so this file's delegate adds the timed,
                           physical part chef doesn't need: HandleMovement()
                           records the direction, and a shared FreeRTOS task
                           drives two relay outputs (UP/DOWN, active-LOW,
                           mutually exclusive by construction) and reports
                           CurrentPositionLiftPercent100ths periodically via
                           linear interpolation against a calibrated
                           full-travel time (WINDOW_COVERING_FULL_TRAVEL_MS)
                           — no position sensor assumed, same time-based
                           technique ESPHome's/Tasmota's own cover
                           components use for this class of cheap motor
                           hardware. An extra overshoot allowance runs the
                           motor a bit longer than the calibrated time when
                           the target is a hard 0%/100%, so timer jitter
                           doesn't leave the covering short of its actual
                           physical end stop. Position is therefore only as
                           accurate as the calibration — a stalled/slipped/
                           hand-moved covering will silently drift out of
                           sync until the next full open or close command
                           re-anchors it to a known endpoint. A real,
                           substantive bug was caught by an actual Docker
                           build, not by inspection: esp-matter's own
                           `nullable<T>` wrapper (used for the position
                           config fields) isn't the same type as
                           `chip::app::DataModel::Nullable<T>` — the first
                           build attempt used the latter and failed to
                           compile; fixed by using esp-matter's own
                           `nullable<uint16_t>(0)` constructor instead.
                           Build-verified in Docker; not hardware-tested (no
                           motor/relay hardware for this device type
                           physically available when written).
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
   both, not just light.

   A fourth device type, `firmware/outlet/` (`on_off_plug_in_unit`), was
   added after this, alongside `firmware/switch/` rather than replacing
   it — see its own repository-layout entry above for why, and
   `tools/product-wizard/README.md` for the full Apple Home
   icon/commissioning story for both.

   A fifth, `firmware/temperature-sensor/`, followed: originally a
   Sensirion SHT3x over I2C only (this repo's first non-GPIO sensor and
   first multi-endpoint device — temperature + humidity, since Matter has
   no single device type for both from one sensor chip). Reading it
   needed ESP-IDF's newer `driver/i2c_master.h` API (verified against
   this exact SDK version's headers rather than assumed) and hit the
   identical "code-driven cluster, not generic `attribute::update()`"
   issue contact-sensor's BooleanState did — same fix
   (`SetMeasuredValue()` via the registry), confirmed by reading
   `TemperatureMeasurementCluster.h` / `RelativeHumidityMeasurementCluster.h`
   directly rather than guessing the setter's name or signature.

   Extended afterwards to 7 selectable sensors (`#define SENSOR_TYPE`):
   SHT3x, SHT4x, AHT20, DHT11, DHT22, DS18B20, BME280 — prompted by
   realizing the first list only covered sensors this repo's own testing
   happened to have on hand, which nearly excluded BME280 despite it
   being confirmed (via web search, not assumed) as the ESP32/ESPHome/
   Home Assistant community's most-recommended all-rounder. Each is a
   genuinely different protocol (I2C with different command sets,
   single-wire bit-banged, or 1-Wire), verified against multiple
   independent sources before writing any timing-critical code (DHT/
   DS18B20 bit timings, AHT20/SHT4x I2C command bytes and conversion
   formulas, BME280's calibration register map) rather than trusted from
   memory. SHT3x, DHT11, and DHT22 are verified on real hardware — DHT11
   and DHT22 worked correctly on the first flash, no debugging needed,
   confirming the shared bit-banged driver's timing is right. SHT4x,
   AHT20, DS18B20, and BME280 are implemented from their datasheets/
   reference drivers but not personally hardware-tested here — flagged as
   such in the code, the wizard's Configure Device step, and its Generate
   Firmware step.

   Also fixed while testing this: SHT3x's readings (27.1–27.9 °C) stayed
   stable even after moving the sensor away from the board, so an initial
   offset from a cheap reference sensor nearby (26.1 °C / 48 %RH vs. our
   27.2 °C / 56 %RH) is most likely that reference's own lower accuracy,
   not a bug — SHT3x is spec'd tighter (±0.2 °C / ±2 %RH) than typical
   low-cost sensors.

   The wizard's `DEVICE_TYPES` schema only supported one configurable
   secondary GPIO (`button`, added for the outlet) before this — I2C needs
   two pins (SDA + SCL), so that field was generalized to `secondary`
   (label-driven UI text, not hardcoded to "button") rather than adding a
   third bespoke field; the outlet's own behavior is unchanged. A new
   `sensorModels` list + dropdown was added on top of that for this
   device type specifically, driving a third sed target
   (`#define SENSOR_TYPE ...`) alongside the driver/secondary GPIO
   fields; see the comment on the `DEVICE_TYPES` entry in `index.html`.

   Also caught and fixed a real, previously-unnoticed wizard bug while
   testing this: switching device type on an already-configured product
   left the old type's GPIO values behind instead of resetting them — a
   product configured as the outlet (driver default GPIO 2) and then
   switched to the temperature sensor kept silently showing GPIO 2 as the
   SDA pin instead of the correct default of 21, with no indication
   anything was wrong. Fixed by resetting gpioPin/secondaryGpioPin/
   identifyGpioPin/component whenever the type actually changes.

   A sixth device type, `firmware/light-sensor/` (`light_sensor`
   device type, Illuminance Measurement cluster), followed: this repo's
   first analog/ADC device — an LDR/photoresistor voltage divider on
   ESP32's ADC, converted to lux via the standard photoresistor
   characteristic curve and then to Matter's logarithmic MeasuredValue
   encoding. See its own repository-layout entry above for the details.
   Not hardware-verified in this repo (no LDR on hand when written),
   flagged as such throughout, same standard as the temperature sensor's
   unverified chips.

   Also prompted by this: the wizard's `sensorModels`/`sensorModel`
   naming (added for the temperature sensor) was generalized into a
   shared top-level `COMPONENT_LIBRARY` (keyed by id) plus a generic
   `component`/`componentOptions` naming throughout, per the user's own
   suggestion — avoids duplicating a component's metadata if more than
   one device type can offer it (e.g. a relay module usable by both the
   outlet and a possible future lamp-style device type with a choice of
   light source). `firmware/light-sensor/` itself doesn't use
   `componentOptions` at first — different LDR models mostly differ by
   two numeric constants (`LDR_R10_OHMS`/`LDR_GAMMA` in its
   `app_main.cpp`, adjustable by hand per its own comment) rather than
   genuinely different drivers, so a selector didn't seem warranted the
   way it was for temperature's different protocols.

   That changed once a second, genuinely different light sensor was
   added: `firmware/light-sensor/` now also supports the **BH1750**, a
   digital ambient light sensor over I2C (almost always sold as a
   "GY-30"/"GY-302" breakout) that reports lux directly — no
   voltage-divider math or per-unit LDR characterization needed, and no
   ADC-capable pin required either. Implemented from the ROHM BH1750FVI
   datasheet's documented instruction set (One Time H-Resolution Mode,
   command `0x20`; `lux = raw / 1.2`), cross-checked against the
   widely-used claws/BH1750 Arduino library's own header before writing
   any code, same verification standard as every other sensor driver in
   this repo. This is exactly the case the `COMPONENT_LIBRARY` refactor
   above was built for: `firmware/light-sensor/` now uses
   `componentOptions: ["LDR", "BH1750"]` + a third sed target
   (`#define SENSOR_TYPE ...`), the identical mechanism
   `firmware/temperature-sensor/` already used, needing zero new wizard
   *architecture* — only a second `COMPONENT_LIBRARY` entry and one
   device-type entry update. The device-type-level `hardwareVerified`
   flag added for the LDR-only version was removed accordingly, since
   the per-component `verified` flag (already `false` for both LDR and
   BH1750 in `COMPONENT_LIBRARY`) now covers the same case at finer
   granularity, same as temperature-sensor's chips.

   All six device types (light, switch, contact sensor, outlet,
   temperature sensor, light sensor) now exist. Five of six have been
   built, flashed, and commissioned via Apple Home through the wizard's
   own generated commands, run verbatim against the real repo — zero
   errors on any of them. The light sensor has been build-verified in
   Docker for both `SENSOR_TYPE` values (LDR and BH1750) and boot-tested
   on real hardware for the LDR path (clean compile, clean boot,
   sensible-looking fallback readings with nothing wired to the ADC pin)
   but not taken through commissioning yet, and BH1750 hasn't been
   boot-tested on real hardware at all (no BH1750 module on hand when it
   was added) — flagged accordingly. Adding a device type with a physical
   actuator beyond simple on/off (e.g. a dimmable/color light, or a
   cover/blind) is a reasonable next one, to cover ground no existing
   device type here does.

   `firmware/switch/` was then extended (not a new device type — a
   capability added to the existing one) to support 1-4 independent
   buttons instead of exactly one, prompted directly by wanting several
   physical buttons on one board, each controlling a different bound
   device. Each button is its own `on_off_light_switch` endpoint — same
   modelling Matter itself uses for a physical multi-gang wall switch.
   `SWITCH_BUTTON_COUNT` (default 1, matching every switch product from
   before this existed) selects how many; build-verified in Docker for
   both 1 (regression) and 4 (new path), but only the original single-
   button/GPIO 4 configuration has been tested on real hardware — flagged
   in the wizard for `SWITCH_BUTTON_COUNT` > 1 accordingly. The wizard
   gained a new "How many buttons?" selector plus up to 3 additional
   per-button GPIO fields (`extraButtons` on the `DEVICE_TYPES` entry,
   `buttonCountDefineName` for the non-GPIO `SWITCH_BUTTON_COUNT` sed
   target — same non-GPIO-pattern mechanism `componentDefineName` already
   used for the temperature/light sensors' `SENSOR_TYPE`) — the first
   device type needing a *variable number* of GPIO fields rather than a
   fixed one or two.

   `firmware/outlet/` was then extended with an optional relay output
   polarity (`OUTLET_OUTPUT_TYPE`: LED active-HIGH default vs. RELAY
   active-LOW) and, much more substantially, optional power monitoring via
   6 selectable chips (`OUTLET_POWER_MONITOR`: BL0942, BL0937, HLW8012,
   CSE7759, CSE7766, ADE7953), spanning three genuinely different
   protocol families — UART request/response, UART auto-report, GPIO
   pulse-frequency, and I2C. This is this repo's first device exposing a
   second Matter endpoint (Electrical Sensor, device type 0x0510) and the
   first to combine two different esp-matter cluster-integration patterns
   in one file: ElectricalPowerMeasurement needed a hand-written
   push-style `Delegate` subclass (adapted from esp-matter's own
   `examples/all_device_types_app` reference code, since the generic
   `cluster::electrical_power_measurement::create()` config is an
   undocumented raw `void*` — not risked); ElectricalEnergyMeasurement,
   by contrast, ships a complete ready-made implementation in esp-matter
   itself, driven entirely through free functions
   (`data_model_provider/clusters/electrical_energy_measurement/
   integration.h`) with no custom Delegate needed at all. Per the user's
   explicit instruction, every chip's protocol/formula was checked
   against its own manufacturer datasheet directly (not just ESPHome's
   open-source implementations, which an earlier draft had leaned on) —
   this caught two real bugs: BL0942's response packet had current and
   voltage at swapped byte offsets, and CSE7766's "Adj" status byte was
   mischaracterized as a per-measurement validity flag when the datasheet
   defines it as "cycle complete vs. partial" (the resulting skip-logic
   behavior was coincidentally already correct; only the comment was
   wrong). BL0937 and HLW8012's existing formulas were independently
   re-derived from their datasheets and confirmed correct as-is. CSE7759
   (assumed to share HLW8012's formula per a secondary source only — its
   own datasheet wasn't obtainable) and ADE7953 (least-certain of the
   six — only partially confirmed against Analog Devices' own datasheet
   across several fetch attempts) are flagged as such everywhere:
   code comments, the wizard's Configure Device sidebar note, and its
   Generate Firmware warning box (with an extra explicit caveat for
   ADE7953 specifically). Build-verified in Docker across all 7
   power-monitor configurations x both output types; not hardware-tested
   (no module of any of the 6 chips was physically available in this
   repo). The wizard gained a new `extraPickers` mechanism on
   `DEVICE_TYPES` — deliberately separate from `componentOptions`, since
   `componentOptions`'s selection also drives GPIO-field labeling
   (`driverLabel`/`secondaryFieldNeeded`) that doesn't apply to an output-
   type or power-monitor-chip choice — to render the Output and Power
   Monitoring pickers as two stacked checkable lists in the same left
   sidebar, each independently keyed and independently sed'd.

   Also, prompted by "does the contact sensor need a sensor list like the
   temperature sensor?": added a purely cosmetic `componentOptions` list
   to `firmware/contact-sensor/` (reed switch / hall sensor / microswitch)
   for visual continuity with the other device types' pickers, even
   though — as `componentsPurelyVisual: true` documents everywhere it's
   read — every option compiles to the exact same driver, since a contact
   sensor is always just HIGH or LOW to the microcontroller regardless of
   which physical part is wired up.

   Real-world feedback on the outlet's relay/power-monitoring feature
   (above) surfaced three gaps, all fixed together: (1) `OUTLET_OUTPUT_TYPE`
   defaulted to LED, but a relay is what an actual power outlet/smart plug
   normally switches with — LED was really only useful for breadboard
   testing without a relay module on hand, so the default (both the
   firmware's own `#define` and the wizard's picker order) flipped to
   RELAY; (2) some real plug hardware has a small status-indicator LED,
   separate from the relay/LED output itself, wired to its own GPIO, that
   continuously mirrors on/off state — distinct from the Identify LED
   (which only blinks temporarily on an Identify command) — added as a new
   optional `OUTLET_STATUS_LED_GPIO` (off by default, `GPIO_NUM_NC`,
   checked at runtime in `set_output()` since that sentinel is a
   `gpio_num_t` enumerator, not a preprocessor macro usable in `#if`), with
   its own opt-in checkbox + GPIO field in the wizard, same shape as the
   Identify LED's own checkbox but defaulting off instead of on; (3) the
   six power-monitor chips' own GPIOs (BL0942/CSE7766's UART pins,
   BL0937/HLW8012/CSE7759's shared SEL/CF/CF1 pulse pins, ADE7953's I2C
   pins) were hardcoded `#define`s the wizard never exposed at all — picking
   a chip in Power Monitoring showed no way to configure the pins it
   actually needs. Fixed by attaching a `pins` array (label, `#define`
   name, per-module defaults) to each power-monitor `COMPONENT_LIBRARY`
   entry and rendering them as extra GPIO fields in Configure Device once
   that chip is selected — same "don't show a field the driver doesn't
   use" principle as the temperature sensor's `usesPin2` (CSE7766 exposes
   only its RX pin, since the chip only transmits and this firmware only
   reads, so the ESP32's own TX line goes nowhere). Build-verified in
   Docker for the default config, status LED enabled, and LED output +
   status LED together; smoke-tested in the wizard across the full
   2 output x 7 power-monitor x 2 status-LED-state matrix, 0 failures.

   A second round of feedback on the same feature followed immediately:
   offering LED as an equally-weighted choice next to Relay in the
   wizard's Output picker was itself misleading, not just wrongly
   defaulted — a relay is simply what an actual outlet/smart plug
   switches with, so the picker was removed outright rather than just
   re-defaulted. `OUTLET_OUTPUT_TYPE` now always builds as the firmware's
   own default (`OUTLET_OUTPUT_RELAY`), un-sed'd, exactly like any other
   `#define` the wizard doesn't expose a field for; LED remains available
   by hand-editing the source for breadboard testing, just not as a
   wizard choice. This left outlet's `extraPickers` array with a single
   entry (Power Monitoring) — confirms `extraPickers` was worth designing
   as an array from the start rather than a fixed two-picker shape.
   Separately, the new Status LED checkbox/field was moved to render
   (and review, and appear in the Configuration summary sidebar) directly
   above the Identify LED block instead of below it — the status LED
   reflects the device's actual ongoing on/off state, so it reads better
   ordered ahead of Identify's one-off blink. Re-smoke-tested after both
   changes (0 failures) and rebuilt in Docker to confirm the LED_OUTPUT/
   RELAY_OUTPUT `COMPONENT_LIBRARY` removal didn't affect anything the
   firmware side depends on (it doesn't — those entries only ever fed the
   now-removed wizard picker).

   A seventh device type, `firmware/dimmable-light/` (`dimmable_light`
   endpoint type), followed — this repo's first device type with a real
   actuator beyond simple on/off, per the user's own choice between that,
   a color light, and a window covering. Adds the LevelControl cluster on
   top of OnOff, driving the output as real PWM via ESP-IDF's
   `driver/ledc.h` (the LEDC hardware peripheral) rather than a plain
   `gpio_set_level()` — confirmed against esp-matter's own
   `endpoint::dimmable_light::add()` for the exact cluster composition,
   and against the SDK's own `examples/light/` reference (app_main.cpp +
   app_driver.cpp) for the CurrentLevel-handling pattern. LevelControl
   turned out to be a plain ember attribute, not a "code-driven" cluster
   class the way BooleanState/TemperatureMeasurement/
   ElectricalPowerMeasurement are elsewhere in this repo — confirmed by
   checking that `data_model_provider/clusters/` has no `level_control/`
   folder — so it needed the exact same `attribute::PRE_UPDATE` +
   `attribute::update()` pattern as OnOff, no special setter. See its own
   repository-layout entry above for the full detail (brightness scaling,
   boot-to-Off convention, deferred NVS persistence for CurrentLevel).
   The wizard needed zero new mechanism for this one — its `DEVICE_TYPES`
   entry is the same single-GPIO-plus-identify shape `firmware/light/`'s
   own entry already uses, confirming that shape really is generic rather
   than accidentally light-specific.

   Taken through a real hardware test immediately after: built and
   flashed via the wizard's own generated commands against an ESP32
   WROOM-32 with an LED on GPIO 2 (this device type's own default pin —
   no `#define` edits needed), commissioned into Home Assistant (full
   PASE/CASE handshake, zero errors), then both On/Off and the brightness
   slider exercised live — every serial-log line
   (`Light turned ON`/`Light turned OFF`/`Light level set to N/254`)
   matched what was actually done in Home Assistant's UI, including a
   rapid slider drag producing a stream of distinct `CurrentLevel`
   updates all correctly reaching the LEDC output. First device type to
   have its dimming behavior specifically confirmed end to end, not just
   commissioning; see its own repository-layout entry above for the full
   detail now that this is hardware-verified, not just build-verified.

   An eighth device type, `firmware/window-covering/` (`window_covering`
   endpoint type), followed — the option not picked when dimmable light
   was chosen from the same three (the other being a color light). This
   repo's first device type with continuous, multi-second physical
   movement rather than an instant response, and the first where the
   Matter cluster itself does *not* drive hardware or simulate movement —
   confirmed directly in esp-matter's source that WindowCovering only
   validates commands and calls an app-supplied Delegate, unlike
   LevelControl (which ramps CurrentLevel entirely inside
   connectedhomeip's own cluster server). Cross-checked against
   connectedhomeip's own real reference delegate implementation
   (`examples/chef/common/clusters/window-covering/`) for the correct
   attribute Get/Set + `MatterReportingAttributeChangeCallback()` pattern
   before writing this file's own delegate, which adds the timed,
   physical movement chef's headless/simulated version doesn't need: two
   relay outputs (UP/DOWN, active-LOW, mutually exclusive by
   construction) driven by a shared FreeRTOS task that estimates position
   via linear interpolation against a calibrated full-travel time — the
   same technique ESPHome's/Tasmota's own time-based cover components use
   for motors with no position sensor of their own. A real compile error
   was caught by an actual Docker build on the first attempt, not by
   inspection: esp-matter's own `nullable<T>` wrapper type isn't the same
   as `chip::app::DataModel::Nullable<T>` — fixed by using esp-matter's
   own constructor. Build-verified in Docker; not hardware-tested (no
   motor/relay hardware for this device type physically available when
   written). See its own repository-layout entry above for the full
   detail, including the documented limitation that position accuracy
   depends entirely on calibration and drifts if the motor stalls, slips,
   or is moved by hand.

   Also during this session: the wizard's Select Module connectivity
   badges were restyled (small bordered rounded-rect instead of filled
   pills, per a reference screenshot) and gained a "Matter" badge (always
   shown — every module here builds Matter firmware) and a "Zigbee"
   badge for the 802.15.4-capable chips, with an explicit note (initially
   in the visible label, moved to the hover tooltip after feedback) that
   the radio can physically run Zigbee but this repo/wizard never
   actually builds Zigbee firmware for it. Three ESP32 modules missing
   from the picker were added — ESP32-C2, ESP32-C5, ESP32-C61 — after
   checking each chip's real connectivity directly in ESP-IDF's own
   `soc_caps.h` rather than assuming from the chip name alone; this
   caught that ESP32-H4 and ESP32-H21 (initially assumed addable) both
   have their BLE/802.15.4 capability defines commented out on this
   repo's exact pinned ESP-IDF version (v5.5.4), so they were left out
   rather than added and silently broken.
2. Implement Matter **OTA** — partially done. All eight firmware types ship
   `CONFIG_ENABLE_OTA_REQUESTOR=y`, which adds the OTA Requestor cluster
   to the root node endpoint entirely via Kconfig — esp-matter's own core
   startup (`esp_matter_core.cpp`) calls
   `esp_matter_ota_requestor_init()`/`_start()` automatically once that
   flag is on, so no app code was needed. Confirmed on real hardware for
   `firmware/contact-sensor/` and `firmware/switch/` (clean boot, cluster
   registered, zero errors); the other five build identically since the
   code path is generic to every device type, not device-specific.

   Still open: a real OTA **transfer** needs an OTA Provider node
   commissioned onto the same fabric, actually serving a `.bin` (e.g.
   `chip-ota-provider-app`, source-only in the esp-matter image, not
   prebuilt) — which needs `chip-tool`-class commissioning tooling, which
   needs BLE, which Docker Desktop on macOS doesn't pass through to
   containers. The original goal of updating "from a GitHub Release
   `.bin`" also needs a small bridge piece that doesn't exist yet:
   something that downloads the release asset and feeds it to whatever
   OTA Provider is running (Matter OTA doesn't fetch arbitrary URLs
   directly — only BDX from a Provider on the fabric). Revisit once
   native macOS `chip-tool` (or equivalent commissioning tooling) exists.
   Signed/encrypted OTA (`esp_matter_ota_requestor_encrypted_init()`, see
   `examples/light/main/app_main.cpp` in the SDK) is a further step after
   that.
3. ~~`firmware/switch/`'s onboard BOOT/PROG button (GPIO 0) was unreliable~~ —
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
