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
firmware/color-light/     RGB/RGBW/RGBWW color light — ninth device type,
                           and the last
                           of the three options offered together when
                           firmware/dimmable-light/ was added (dimmable
                           light and window covering were the other two,
                           both already built)
  main/app_main.cpp       Hand-assembled endpoint (Identify + Groups +
                           OnOff + LevelControl + ColorControl[Hue/
                           Saturation only] + ScenesManagement), not
                           esp-matter's endpoint::extended_color_light::
                           create() — confirmed directly in esp-matter's
                           source that helper unconditionally wires up
                           ColorTemperature *and* Xy features but never
                           HueSaturation, despite that being what most
                           controllers' color wheels (Apple/Google Home,
                           Home Assistant) actually drive. Supporting all
                           three color modes properly needs three separate
                           real colorimetry conversions (HSV→RGB, CIE
                           xyY→RGB, correlated-color-temperature→RGB) —
                           same "smallest reasonable next step" scoping
                           already used for firmware/dimmable-light/
                           (LevelControl only) and firmware/window-covering/
                           (Lift only, no Tilt): this device implements
                           exactly one color mode, correctly, rather than
                           three with two approximate. Device type is still
                           declared as ExtendedColorLight (0x010D) — the
                           correct Matter device type for any color-capable
                           light regardless of which ColorControl features
                           it implements; FeatureMap/ColorCapabilities are
                           both set to HueSaturation-only so a real
                           controller won't offer XY/color-temperature
                           controls this device can't act on. Built by
                           calling esp-matter's own lower-level free
                           functions directly (endpoint::create(),
                           add_device_type(), each cluster's own create()/
                           feature::xxx::add()) — the same public API the
                           higher-level endpoint helper itself is built
                           from, just composed differently. OnOff/
                           LevelControl/ColorControl's CurrentHue/
                           CurrentSaturation are all plain ember
                           attributes (confirmed via esp-matter's own
                           examples/light/main/app_driver.cpp, which
                           reacts to them through the same
                           attribute::PRE_UPDATE pattern used everywhere
                           else in this repo) — no Delegate needed, unlike
                           firmware/window-covering/'s WindowCovering.
                           CurrentHue/CurrentSaturation are both uint8
                           0-254 (confirmed directly in the Matter 1.6
                           ColorControl.xml spec file's own attribute
                           constraints), mapped to degrees/fraction and
                           combined with LevelControl's CurrentLevel (as
                           HSV's "V") through a textbook HSV→RGB
                           conversion, output via three LEDC PWM channels
                           sharing one timer — same LEDC pattern
                           firmware/dimmable-light/ already established,
                           times three. One real compile error caught by
                           an actual Docker build, not inspection: a
                           namespace-qualification slip
                           (`identify::command::create_trigger_effect`
                           instead of `cluster::identify::command::
                           create_trigger_effect`) — fixed, second build
                           clean. #define COLOR_LIGHT_COLOR_MODE (a 3-way
                           enum: COLOR_LIGHT_MODE_RGB/_RGBW/_RGBWW, RGB by
                           default) selects between three hardware
                           variants. RGBW adds a 4th LEDC channel,
                           converting the same Hue/Saturation color to
                           RGBW via the standard "extract common white"
                           technique (W = min(R,G,B), then subtract W
                           from each of R/G/B) — the same algorithm Home
                           Assistant's own color utility
                           (`color_rgb_to_rgbw`) and WLED use, not
                           something invented for this file; Matter's
                           ColorControl cluster has no separate "White"
                           concept at all, this is purely a local
                           hardware-rendering decision. RGBWW (what LED
                           strip vendors sell as "RGBCCT"/"RGB+CCT" — same
                           5-channel hardware) is a genuinely different
                           case, not just one more channel: real RGBCCT
                           products don't blend RGB and white
                           simultaneously (confirmed in ESPHome's own
                           rgbww light component docs, which call this
                           "color_interlock"), which maps cleanly onto
                           Matter's ColorControl cluster already having a
                           `ColorMode` attribute that distinguishes
                           Hue/Saturation from ColorTemperatureMireds as
                           separate color spaces — so RGBWW mode adds the
                           ColorTemperature feature
                           (`cluster::color_control::feature::
                           color_temperature::add()`, config_t field
                           names confirmed directly against esp-matter's
                           own color_control.h) alongside HueSaturation,
                           and the firmware locally latches which color
                           space a controller last commanded, driving
                           either the RGB channels or the cool/warm
                           channels — never both — exactly ESPHome's
                           interlock behavior, just built against
                           Matter's cluster instead. Converting a target
                           ColorTemperatureMireds into cool/warm channel
                           duty cycles reuses ESPHome's own
                           light_call.cpp formula verbatim (clamp into
                           range, linear-interpolate the warm/cool
                           fraction, then normalize both by their max so
                           at least one channel stays at full strength at
                           any color temperature instead of both dimming
                           together at the midpoint) — fetched and
                           confirmed from ESPHome's actual source file,
                           not assumed. COLOR_LIGHT_COOL_WHITE_KELVIN/
                           _WARM_WHITE_KELVIN default to 6500K/2700K, the
                           two most common "daylight"/"warm white" LED
                           bin ratings across the LED lighting industry —
                           explicitly documented as adjustable per your
                           actual LEDs' rated color temperature, since no
                           RGBWW hardware was available to measure real
                           values (ESPHome's own documented RGBWW example
                           uses 6536K/2000K instead, underlining there's
                           no universal default). Wizard integration
                           (tools/product-wizard/) needed the color-mode
                           picker's options to become a true 3-way enum —
                           RGB/RGBW's defineValue changed from raw "0"/
                           "1" to the actual C constant names
                           (COLOR_LIGHT_MODE_RGB/_RGBW), same pattern
                           SENSOR_TYPE/OUTLET_POWER_MONITOR already use —
                           but needed zero new render/validation/sed
                           logic: RGBWW's 2 extra GPIO fields (cool white,
                           warm white) go through the exact same
                           multi-pin `pins` array mechanism BL0942's UART
                           pins and ADE7953's I2C pins already exercise,
                           confirmed by reading that code path rather
                           than assuming it only handled one pin. Build-
                           verified in Docker for all three modes (RGB,
                           RGBW, RGBWW); not hardware-tested (no RGB(W)(W)
                           LED/driver board for this device type
                           physically available when written). The wizard's
                           Configure Device sidebar also gained a second,
                           purely cosmetic picker — "If you're actually
                           using an addressable chip," listing the same 8
                           chips firmware/addressable-light/ actually
                           supports (WS2812B/WS2813/WS2815/SK6812/SK6812
                           RGBW/WS2805/APA102/SM2335EGH) — since Color
                           Light's plain LEDC PWM output physically cannot
                           speak any of those chips' real protocols
                           (single-wire NRZ timing or SPI), this is
                           reference-only: no #define exists for it, so
                           the new `cosmetic` flag on an `extraPickers`
                           entry (a small, reusable generalization, not a
                           one-off) tells renderConfigureDevice/
                           buildSedCommands/renderCustomiseReview to
                           render it normally but skip generating any sed
                           command or "verified"/"build-tested" framing
                           for it — same "shows in the UI, zero effect on
                           the generated firmware" contract
                           `componentsPurelyVisual` already established for
                           contact-sensor's reed switch/Hall sensor list,
                           reused here rather than inventing a second,
                           differently-shaped mechanism for the same idea.
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/addressable-light/  Addressable LED strip / smart-bulb driver —
                           tenth device type, and this repo's first over
                           addressable/digital LED protocols (single-wire
                           NRZ via RMT, real SPI for APA102, or bit-banged
                           2-wire for SM2335EGH) rather than plain PWM
  main/app_main.cpp       Same hand-assembled ExtendedColorLight endpoint
                           as firmware/color-light/ (Identify + Groups +
                           OnOff + LevelControl + ColorControl[HueSaturation,
                           +ColorTemperature for the two RGBCCT chips] +
                           ScenesManagement) — copied from that file,
                           changed only where the addressable output needs
                           something different from LEDC PWM. Crucial
                           scoping decision, verified rather than assumed:
                           "addressable" does NOT mean per-pixel/RGBIC-style
                           control here — every pixel/fixture is always set
                           to the same color, because Matter itself has no
                           ratified way to ask for anything else. Checked
                           directly in connectedhomeip's own
                           controller-clusters.matter: there IS a
                           `provisional cluster DynamicLighting = 773`
                           (0x0305) with EffectStruct/EffectColorStruct
                           types that look exactly like what real per-pixel
                           effects would need — but it's marked
                           `provisional` and absent from every ratified
                           data_model spec folder checked (1.0 through
                           1.6), so no real controller (Apple/Google Home,
                           Home Assistant) can command it today, and
                           esp-matter has no cluster support for it either.

                           Grew from 2 chips to 8 across three protocol
                           families on request (WS2812B, SK6812, SK6812
                           RGBW, WS2813, WS2815, APA102, then — after a
                           follow-up screenshot of a real manufacturing
                           tool's "Device Drivers" screen — WS2805 and
                           SM2335EGH). #define ADDRESSABLE_LIGHT_CHIP
                           selects between them:

                           Six single-wire NRZ chips (WS2812B, WS2813,
                           WS2815, SK6812, SK6812_RGBW, WS2805) — every
                           one independently verified against Worldsemi's
                           own datasheet (not a secondary source; fetched
                           as PDFs and read via `pdftotext`, since a plain
                           web search turned up two conflicting "official"
                           WS2812B timing tables and vaguer, wider-ranged
                           numbers for WS2813/WS2815 in the process).
                           WS2812B: T0H=0.4us/T0L=0.85us/T1H=0.8us/
                           T1L=0.45us, reset>=50us, GRB order. WS2813/
                           WS2815: their own datasheets cite noticeably
                           wider tolerance windows that do NOT simply
                           contain WS2812B's own values (e.g. WS2812B's
                           400ns T0H sits just outside WS2813/WS2815's
                           cited 220-380ns window) — caught by actually
                           checking the numeric ranges rather than
                           assuming timing-compatibility from community
                           reputation, so this pair gets its own tailored
                           constants (T0H=0.3us/T0L=0.8us/T1H=0.8us/
                           T1L=0.3us) chosen to sit inside both chips'
                           real windows. SK6812/SK6812_RGBW: confirmed
                           identical timing across both chips' own
                           datasheets (T0H=0.3us/T0L=0.9us/T1H=0.6us/
                           T1L=0.6us, reset=80us) — plain SK6812 is GRB,
                           RGBW is RGBW order, flagged in a code comment
                           as a real point of disagreement with several
                           community Arduino/ESPHome libraries that
                           default to GRBW instead. WS2805 (fetched
                           directly from world-semi.com, not a distributor
                           mirror): T0H=220-380ns/T1H=580ns-1us/
                           T0L=580ns-1us/T1L=580ns-1us, reset>280us — note
                           T1L is long here, not short like the WS2812B-
                           family above, so it gets its own values too
                           (T0H=0.3us/T0L=0.8us/T1H=0.8us/T1L=0.8us);
                           RGBW1W2 byte order, W1=warm/W2=cool assumed by
                           convention (not itself labelled in the
                           datasheet). ADDRESSABLE_LIGHT_RESET_US (300us)
                           is shared by all six — safely above every one
                           of their individual minimums (50-280us), since
                           the reset only costs time once per full-strip
                           update. Driven via ESP-IDF's `driver/rmt_tx.h`
                           — the first driver in this repo to use RMT
                           rather than bit-banged GPIO timing (unlike
                           firmware/temperature-sensor/'s DHT11/DHT22/
                           DS18B20, which predate this) — exact API
                           pattern checked against Espressif's own
                           official `examples/peripherals/rmt/
                           led_strip_simple_encoder` reference (which
                           lists classic ESP32 among its supported
                           targets), only the timing constants/byte order
                           differing, sourced from the datasheets above.

                           APA102 (DotStar) — not single-wire NRZ at all:
                           a real 2-wire clock+data interface (SPI without
                           chip-select), driven via ESP-IDF's real
                           `driver/spi_master.h` instead of RMT, the first
                           real-SPI driver in this repo. APA102's own
                           datasheet is notoriously thin on protocol
                           detail, so this follows the widely-cited,
                           independently-verified cpldcpu.com
                           reverse-engineering writeup instead (cross-
                           checked against Adafruit's/SparkFun's own
                           guides, which agree) — same "best available
                           source, explicitly flagged" precedent as
                           CSE7759 in firmware/outlet/. Frame: 32-bit
                           zero start frame, one 32-bit brightness-
                           prefixed-BGR frame per pixel (brightness
                           always sent at max/31 per that source's own
                           explicit recommendation — brightness lives in
                           R/G/B via HSV's "V" instead, same as every
                           other chip here), then a pixel-count-aware end
                           frame (>= ceil(pixel_count/2) one-bits) instead
                           of the datasheet's fixed 32-bit one, which the
                           same source documents as only reliable up to 64
                           LEDs.

                           SM2335EGH — architecturally different from
                           every other chip here: a single-fixture,
                           5-channel (RGB+CW+WW) smart-bulb driver IC, not
                           a pixel-chain chip at all, so
                           ADDRESSABLE_LIGHT_PIXEL_COUNT doesn't apply to
                           it (the wizard hides that field when it's
                           selected — see below). No real protocol
                           datasheet exists for it — confirmed directly by
                           fetching the manufacturer's (chinaasic.com) own
                           "datasheet," which turned out to be a one-page
                           feature summary with zero protocol detail, and
                           by multiple independent open-source driver
                           authors (ESPHome, the sm2335egh-rs Rust crate)
                           documenting the same experience asking the
                           manufacturer directly. This file therefore
                           follows ESPHome's own real, open-source,
                           hardware-tested implementation verbatim
                           (`esphome/components/sm10bit_base/
                           sm10bit_base.cpp`, fetched directly): a
                           bit-banged 2-wire (DATA+CLK) protocol, ~2us per
                           bit, 12-byte buffer (model ID 0xC0 + start
                           address + gain byte + 5×10-bit RGB+W1+W2
                           channel values), including an "ACK" clock pulse
                           per byte whose value is never actually checked
                           — matching ESPHome's own implementation exactly
                           rather than guessing at a "cleaner" protocol.

                           WS2805 and SM2335EGH are both genuinely RGBCCT
                           (independent warm/cool white, not just "one
                           more white channel" the way SK6812_RGBW is) —
                           both reuse firmware/color-light/'s
                           COLOR_LIGHT_MODE_RGBWW design wholesale: Matter's
                           ColorTemperature feature added alongside
                           HueSaturation, plus a local color-space
                           interlock (light_color_source) so a controller's
                           most recent command (Hue/Saturation vs. Color
                           Temperature) decides whether the RGB or the
                           W1/W2 bytes get driven, never both — same
                           interlock concept, same mireds-to-duty formula
                           (from ESPHome's light_call.cpp), just re-applied
                           against pixel/frame bytes instead of LEDC
                           channels.

                           Identify LED defaults to the SAME GPIO as the
                           data pin (was GPIO 15 as a separate LED;
                           changed to GPIO 2 = the data pin, on request) —
                           flashes the whole strip/fixture for Identify
                           instead of a separate LED. This could NOT be
                           done the naive way (a second `gpio_config()`
                           call on the same pin, like firmware/color-light/
                           does between its Identify LED and a color
                           channel) since the data pin here is owned by a
                           whole peripheral (RMT channel, SPI bus, or the
                           bit-banged protocol) — a second plain-GPIO
                           config call on that pin would fight the
                           peripheral for ownership and corrupt the
                           output. Fixed with a runtime check
                           (`identify_via_strip`, computed once at
                           startup) rather than `#if`, since GPIO_NUM_*
                           values are plain C enum constants, not
                           preprocessor macros — an `#if` comparison
                           between them would silently evaluate as if both
                           were 0, a real class of bug worth remembering
                           for any future GPIO-equality check in this repo.

                           Wizard integration: the 8-chip picker itself
                           needed zero new mechanism (same componentOptions
                           pattern as before); APA102/SM2335EGH's second
                           (clock) pin reuses the temperature sensor's
                           existing usesPin2/`secondary` mechanism, not a
                           new one. SM2335EGH's missing pixel-count field
                           needed one small, genuinely reusable addition:
                           `hidesNumberField` on a COMPONENT_LIBRARY entry,
                           checked everywhere `numberField` is
                           read (renderConfigureDevice, isProductComplete,
                           buildSedCommands, renderCustomiseReview) so a
                           chip can opt out of a device-type-level field
                           it doesn't use — the same "don't show a field
                           the driver doesn't use" principle as usesPin2,
                           generalized to numberField too. Also caught and
                           fixed two real, pre-existing gaps while wiring
                           this up: `renderCustomiseReview`'s own secondary-
                           pin review row never checked usesPin2 at all
                           (would have shown a misleading "Clock: GPIO 4"
                           row even for chips that don't use one), and the
                           picker's "verified" framing in the left sidebar
                           would have called a purely cosmetic chip choice
                           (see color-light's own entry) "not personally
                           tested... build-verified only" — technically
                           true but misleading, since nothing was ever
                           built with that choice at all. Both fixed with
                           the same usesPin2/`cosmetic`-aware checks.
                           #define ADDRESSABLE_LIGHT_PIXEL_COUNT (default
                           8) is wizard-configurable via a new, generic
                           `numberField` mechanism (not GPIO, not an enum
                           choice — the first plain-integer field type this
                           wizard has), reusable by any future device type
                           needing one. Build-verified in Docker for all 8
                           chips. WS2812B and SK6812_RGBW are now hardware-
                           verified end to end (WS2812B: real 12-pixel
                           strip; SK6812_RGBW: real 3-pixel strip, its own
                           white-channel RGB->RGBW extraction specifically
                           confirmed via a ColorTemperature command driving
                           the strip visibly whiter — both on GPIO 2,
                           commissioned and controlled live via Apple Home
                           — see "Open next steps" for the full debugging
                           story, which also fixed two real endpoint-
                           construction bugs shared with
                           firmware/color-light/); the other 6
                           chips remain build-verified only (no hardware
                           for them was physically available).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/thermostat/      Thermostat (Heat + Cool) — eleventh device type,
                           and this repo's first with a genuine control
                           loop (compares a measured value against a
                           setpoint and drives an output) rather than a
                           direct command pass-through or a plain sensor
                           readout
  main/app_main.cpp        `endpoint::thermostat::create()` — confirmed
                           this is a complete, directly usable esp-matter
                           helper (Identify + Groups + Thermostat cluster),
                           unlike firmware/color-light/'s
                           extended_color_light helper which had real
                           gaps needing hand-assembly; checked by reading
                           esp_matter_endpoint.cpp's own thermostat::add()
                           directly. One real gap noted anyway: its
                           config_t declares an unused scenes_management
                           field that add() never wires up — harmless,
                           documented, not patched around. Feature scope
                           is Heat + Cool (ControlSequenceOfOperation =
                           CoolingAndHeating), not AutoMode/Occupancy/
                           Setback/schedule-configuration/presets — same
                           "smallest reasonable next step" scoping as
                           firmware/dimmable-light/ (Level only) and
                           firmware/window-covering/ (Lift only).
                           SystemMode/ControlSequenceOfOperation/
                           OccupiedHeatingSetpoint/OccupiedCoolingSetpoint/
                           LocalTemperature are all plain ember
                           attributes, not the "code-driven" cluster class
                           firmware/temperature-sensor/'s
                           TemperatureMeasurement is — confirmed via the
                           same check as always (no thermostat/ folder
                           under data_model_provider/clusters/) — so this
                           uses the same attribute::PRE_UPDATE +
                           attribute::update() pattern as OnOff/
                           LevelControl/ColorControl elsewhere, no
                           SetMeasuredValue()-style setter needed. Boots
                           to SystemMode=Off, matching every other device
                           type's boot-to-known-state convention (a
                           heating/cooling system silently staying active
                           across a power cycle is a worse default than a
                           light staying on). Local temperature reuses
                           firmware/temperature-sensor/'s exact 7-chip
                           SENSOR_TYPE driver library verbatim (only
                           LocalTemperature is pushed into Matter;
                           humidity, where the chip provides it, is read
                           but has nowhere to go on a Thermostat cluster).
                           Control is a plain hysteresis (bang-bang) loop
                           — THERMOSTAT_HYSTERESIS_CENTIDEGREES (0.3 degC
                           default) prevents relay/boiler chatter right at
                           the setpoint, the same standard technique every
                           real thermostat uses.

                           #define THERMOSTAT_OUTPUT_TYPE selects one of
                           three genuinely different ways heat/cool demand
                           actually reaches the boiler/AC, all requested
                           together (not staged) because a real European
                           room thermostat needs to support any of them
                           depending on the installation:
                           RELAY (default) — two active-LOW GPIO relay
                           outputs (matching firmware/outlet/'s own relay
                           convention), wired in series with a boiler's/
                           AC's own volt-free "room thermostat" contact
                           loop — the simple 2-wire "T1-T2" input
                           virtually every European CV/gas-combi boiler
                           has, where the boiler itself supplies the
                           voltage and just needs the loop closed to call
                           for heat, the same way a classic mechanical/
                           bimetal thermostat has always worked. No
                           separate power needed on this side.
                           BINDING — sends real OnOff::On/Off commands
                           (not Toggle — a specific target state, since
                           demand must stay correct even if a command is
                           missed) to whatever this endpoint's Binding
                           cluster is bound to, via the exact
                           client::cluster_update() +
                           client::interaction::invoke::send_request()
                           pattern firmware/switch/'s buttons already use.
                           Binding + a client OnOff cluster are added onto
                           this SAME thermostat endpoint (not a second
                           one) — the exact cluster pair esp-matter's own
                           on_off_light_switch::add() uses for its
                           equivalent purpose, confirmed by reading that
                           function rather than guessed. Covers the "a
                           small receiver module hangs next to the
                           boiler, wired to it, and the thermostat tells
                           that module when to call for heat" European
                           installation style — that receiver module
                           needs no new firmware at all, firmware/outlet/'s
                           existing relay output already does the job.
                           Heat-only (this mode's real motivation is
                           specifically the heating-boiler scenario); Cool
                           demand needs RELAY or OPENTHERM.
                           OPENTHERM — a full OpenTherm master for
                           modulating (not just on/off) boiler control.
                           Protocol verified directly against the
                           OpenTherm Association's own "OpenTherm Protocol
                           Specification v2.2" PDF (fetched from
                           ihormelnyk.com and read via pdftotext, this
                           repo's established practice for primary-source
                           specs — a web summary alone materially
                           underspecifies the exact bit-timing/frame-
                           format detail this needs): 34-bit frames (1
                           start + 32 data + 1 stop), 1 parity + 3
                           MSG-TYPE + 4 spare + 8 DATA-ID + 16 DATA-VALUE
                           bits, Manchester/Bi-phase-L encoding at 1000
                           bps (bit '1' = active-to-idle transition, bit
                           '0' = idle-to-active), master transmits via
                           LINE VOLTAGE modulation (idle <=7V/active
                           15-18V) while the slave transmits via LINE
                           CURRENT modulation (idle 5-9mA/active
                           17-23mA) — confirmed as the reason bare GPIO
                           can't drive this bus directly, unlike this
                           repo's other bit-banged protocols (DHT/
                           DS18B20/WS2812B): THERMOSTAT_OPENTHERM_IN_GPIO/
                           _OUT_GPIO connect to a real OpenTherm adapter
                           board's logic-level pins, not the 2-wire bus
                           itself. GPIO-level driver logic (bit-banged TX
                           via esp_rom_delay_us, an edge-interrupt-driven
                           RX state machine) is ported from Ihor Melnyk's
                           opentherm_library (github.com/ihormelnyk/
                           opentherm_library, MIT-licensed) — the
                           reference implementation this whole DIY/
                           ESPHome/Home-Assistant OpenTherm community has
                           standardized on, itself built against the same
                           spec — same "best available, independently
                           cross-checked" sourcing standard already used
                           in this repo for APA102/SM2335EGH. Sends
                           data-id 0 (Status: CH/Cooling-enable bits),
                           data-id 1 (Control setpoint — a fixed CH
                           flow-temperature value while CH is enabled,
                           matching the spec's own documented "on-off
                           control mode" rather than full weather-
                           compensated modulation, which is out of
                           scope), data-id 16 (Room Setpoint), data-id 24
                           (Room temperature) every
                           THERMOSTAT_OPENTHERM_POLL_INTERVAL_MS (1s
                           default — the spec requires the master to
                           communicate at least once a second or a
                           compliant boiler falls back to basic on/off
                           compatibility mode); reads back data-id 25
                           (Boiler temperature) and data-id 17 (Relative
                           modulation level) purely for diagnostic
                           logging. A real, Docker-build-caught issue
                           (not hypothetical): the SPI display driver
                           below pushed this device type's IRAM usage 68
                           bytes over budget — fixed via
                           CONFIG_SPI_MASTER_ISR_IN_IRAM=n in
                           sdkconfig.defaults (a real ESP-IDF Kconfig
                           option trading slightly higher SPI interrupt
                           latency during flash-cache-miss windows for
                           the IRAM headroom back — an easy trade for a
                           display that only redraws every 2s).

                           Optional rotary encoder (off by default, three
                           GPIO_NUM_NC pins) for local setpoint control
                           without a controller: rotate to adjust
                           whichever setpoint the current SystemMode uses
                           (heating in Heat, cooling in Cool), press to
                           cycle Off -> Heat -> Cool -> Off. Standard
                           two-channel quadrature decoding (channel A's
                           falling edge as trigger, channel B's level at
                           that instant gives direction) — the common
                           "sample the other channel on one edge"
                           technique used by countless KY-040-class
                           encoder drivers, not a chip-specific protocol
                           needing external verification. Local changes
                           write straight into the real Matter attributes
                           (esp_matter_uint8()/esp_matter_int16() +
                           attribute::update(), the same calls a
                           controller's own write would trigger) so a
                           bound controller sees them too, not just
                           whatever a local display shows.

                           Optional local display (#define DISPLAY_TYPE,
                           off by default) — three genuinely different
                           chips/protocols, requested together mid-
                           session alongside the rotary encoder and the
                           local-temperature-sensor reuse: GC9A01 (1.28"
                           round, 240x240, SPI) — its own datasheet
                           documents the standard MIPI-DCS commands but
                           leaves roughly 70% of its actual power-on init
                           sequence undocumented (a real, independently
                           confirmed gap, same situation as
                           firmware/addressable-light/'s APA102/
                           SM2335EGH) — its init sequence is ported
                           verbatim from moononournation/Arduino_GFX, a
                           real, widely-used open-source Arduino graphics
                           library, rather than guessed. ST7789 (the
                           "1.25 inch" 76x284 bar module) — fully,
                           properly documented (Sitronix's own
                           datasheet), no undocumented registers needed;
                           the 76x284 active-area offset within the
                           chip's larger addressable range is this
                           specific module's own glass panel, not the
                           chip, so DISPLAY_ST7789_COL_OFFSET/
                           _ROW_OFFSET are left as adjustable #defines
                           rather than hardcoded. SSD1306 (0.96" 128x64
                           I2C OLED) — this repo's first I2C display,
                           its own dedicated bus/pins (not shared with
                           the temperature sensor's, even if that sensor
                           is also I2C) to avoid address/bus-sharing
                           complexity; one of the most standardized init
                           sequences in hobby electronics, checked
                           directly against that near-universal pattern.
                           Rendering is deliberately NOT a bitmap-font
                           text renderer — just large 7-segment-style
                           digits (drawn as filled rectangles, no font
                           table needed) for the temperature reading,
                           plus a colored border/bar (or, on the
                           monochrome SSD1306, a filled-vs-outline bar)
                           for SystemMode + heat/cool demand instead of a
                           text label — closer to how a real thermostat
                           like Nest indicates state than text would be,
                           and a meaningful scope reduction versus a full
                           font renderer.

                           All three THERMOSTAT_OUTPUT_TYPE values, both
                           with and without the rotary encoder, and all
                           three DISPLAY_TYPE values (plus the DISPLAY_NONE
                           default) are Docker build-verified; not
                           hardware-tested (none of this device type's
                           hardware — OpenTherm adapter board, rotary
                           encoder, or any of the three displays — was
                           physically available when written).

                           Wizard integration surfaced one real, previously
                           latent structural gap: renderConfigureDevice's
                           left-sidebar picker rendering was an if/else-if
                           chain treating componentOptions (sensor model),
                           hasVariableButtonCount, and extraPickers as
                           mutually exclusive — true for every device type
                           until this one, which genuinely needs a sensor
                           picker AND two extraPickers (Output, Display)
                           at once. Restructured so componentOptions/
                           hasVariableButtonCount (still mutually exclusive
                           with each other — no device type has needed
                           both) can coexist with extraPickers in the same
                           left sidebar, separated by the same `<hr>` rule
                           extraPickers' own multiple entries already use.
                           The rotary encoder's checkbox+3-GPIO-fields
                           block is its own new `rotaryEncoder` field +
                           `makeRotaryEncoder()` factory with its own
                           render/validate/sed/review wiring, matching
                           this repo's established copy-and-adapt
                           convention rather than a generalized "array of
                           optional pin blocks" refactor.
                           Output/Display both reuse the existing
                           extraPickers `pins`-per-option mechanism
                           unchanged (RELAY: 2 pins, BINDING: 0 pins,
                           OPENTHERM: 2 pins; GC9A01/ST7789: 5 SPI pins
                           each, SSD1306: 2 I2C pins, DISPLAY_NONE: 0
                           pins) — zero new mechanism needed there, only
                           new COMPONENT_LIBRARY entries. This device
                           type's unusually large number of optional
                           peripherals (sensor + relay/OpenTherm +
                           encoder + SPI display) simply runs out of the
                           classic ESP32's ~17 usable
                           GPIOs otherwise.
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/, plus
                           CONFIG_SPI_MASTER_ISR_IN_IRAM=n (see above)
firmware/camera/          Matter Camera — twelfth device type, and the
                           first that doesn't follow this repo's own
                           "one ESP32 chip, one self-contained
                           firmware image, no external SDKs" pattern at
                           all — a verbatim copy of esp-matter's own
                           reference `examples/camera` (Public Domain/
                           CC0 per its own file headers), reproduced
                           here rather than rewritten because
                           reimplementing ~5,300 lines of production
                           WebRTC/Matter integration code from scratch
                           would be both infeasible in any reasonable
                           time and strictly worse than reusing
                           Espressif's own tested implementation — the
                           same "port a real, working reference rather
                           than guess" principle already used for
                           SM2335EGH/APA102 in
                           firmware/addressable-light/ and OpenTherm in
                           firmware/thermostat/, just at a much larger
                           scale. See firmware/camera/README.md's own
                           preamble (added by this repo, everything
                           after it is Espressif's own unmodified
                           README) for the full detail; summarized here:
  main/app_main.cpp        WebRTCTransportProvider (real SDP offer/
                           answer + ICE candidate exchange — the actual
                           Matter cluster commands: SolicitOffer/
                           ProvideOffer/ProvideAnswer/
                           ProvideICECandidates/EndSession) +
                           CameraAvStreamManagement (audio/video/
                           snapshot features, up to 1080p/120fps,
                           kMaxNetworkBandwidthbps 128Mbps) — genuine
                           production camera specs, not a toy example,
                           confirmed by reading camera-device.h's own
                           constants directly. Needs real Matter
                           signaling AND real video capture/H.264
                           encoding running at once — more than any
                           single chip this repo otherwise targets can
                           do, so Espressif's own answer (and this
                           file's) is a **two-chip split architecture**:
                           an ESP32-P4 (camera + hardware video encode)
                           and an ESP32-C6 (Wi-Fi/BLE + Matter), both on
                           one **ESP32-P4 Function EV Board**, talking
                           over SDIO. `firmware/camera/` is only the
                           **ESP32-C6 signaling half** — builds for
                           `esp32c6` (or `esp32c5`), not this repo's
                           default `esp32` target. The ESP32-P4 media
                           half is not part of this repo at all — it's
                           the KVS SDK's own `streaming_only` example,
                           built straight from that externally-cloned
                           SDK per Espressif's own instructions.
                           Needs a real external SDK dependency, unlike
                           every other device type here: the [Amazon
                           Kinesis Video Streams WebRTC
                           SDK](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c)
                           (`beta-reference-esp-port` branch, with its
                           own submodules — libwebsockets, libsrtp2,
                           usrsctp, the KVS PIC/producer-c libraries),
                           cloned separately and pointed to via
                           `KVS_SDK_PATH` — this repo doesn't vendor or
                           bundle it, per the same "no hidden code"
                           principle everything else here follows: you
                           clone the dependency yourself, the same way
                           you already clone this repo and pull the
                           Docker image. Genuinely build-verified, not
                           assumed: built successfully for `esp32c6` in
                           the pinned `espressif/esp-matter:
                           release-v1.6_idf_v5.5.4` Docker image with
                           `KVS_SDK_PATH` pointing at a real, freshly
                           cloned + submodule-initialized (`git
                           submodule update --init --depth 1`, per
                           Espressif's own instructions) copy of that
                           SDK. Not hardware-tested — an ESP32-P4
                           Function EV Board was not physically
                           available when this was added; the first
                           device type in this repo where even getting
                           hardware to test on is several tiers more
                           specialized/expensive than everything else
                           here. Not offered in `tools/product-wizard/`
                           — its whole data model assumes one device
                           type = one chip = one firmware image on one
                           board, which a two-chip/two-firmware/
                           external-SDK device fundamentally doesn't
                           fit; build and flash this one by hand,
                           following Espressif's own instructions in
                           firmware/camera/README.md.
  main/camera-device.cpp,  the actual `CameraDeviceInterface`/
  main/camera-device.h      `CameraHALInterface` implementation +
                           delegate wiring for both clusters —
                           Espressif's own code, unmodified.
  main/clusters/            `CameraAvStreamManagement` and
                           `WebRTCTransportProvider` cluster delegate
                           implementations — Espressif's own code,
                           unmodified.
  main/webrtc/              the actual WebRTC signaling/transport glue
                           against the KVS SDK's own APIs — Espressif's
                           own code, unmodified.
  README.md                 Espressif's own README (build/flash
                           instructions for both halves), with a new
                           preamble section (added by this repo) up top
                           explaining all of the above before their own
                           content starts.
  CMakeLists.txt,           Espressif's own build configuration,
  main/CMakeLists.txt,      unmodified — deliberately NOT adapted to
  partitions.csv,           this repo's own simplified CMakeLists
  sdkconfig.defaults*        pattern (the one every other device type
                           here uses, which explicitly avoids
                           `$ESP_MATTER_PATH/examples/common` — see
                           CLAUDE.md's own "Build (inside the
                           container)" section above) since this
                           example's working, tested build already
                           depends on that shared infrastructure
                           (device_hal/device paths, examples/common)
                           in ways this repo's simpler pattern was never
                           designed to replace.
firmware/door-lock/       Door Lock — thirteenth device type, back to this
                           repo's normal one-chip/one-firmware pattern
                           after firmware/camera/'s exception. First
                           device type where the main command (LockDoor/
                           UnlockDoor) is handled through a plain C
                           weak-symbol override rather than either the
                           attribute::PRE_UPDATE pattern (OnOff/
                           LevelControl/ColorControl/Thermostat elsewhere)
                           or a C++ Delegate class (WindowCovering,
                           WebRTCTransportProvider in firmware/camera/).
  main/app_main.cpp        endpoint::door_lock::create() (Identify +
                           DoorLock cluster) confirmed complete/directly
                           usable by reading esp_matter_endpoint.cpp's
                           own door_lock::add(). DoorLock confirmed NOT a
                           "code-driven" cluster class (no door_lock/
                           folder under data_model_provider/clusters/,
                           unlike e.g. humidistat/) — LockState/LockType/
                           ActuatorEnabled/OperatingMode are all plain
                           ember attributes. esp-matter's own
                           door_lock::config_t has an OPTIONAL `delegate`
                           pointer, left null here (PIN/credential/
                           schedule management is out of scope, same
                           "smallest reasonable next step" precedent as
                           firmware/dimmable-light/'s Level-only scope or
                           firmware/window-covering/'s Lift-only scope).
                           With no delegate, connectedhomeip's own
                           DoorLockServer still handles LockDoor/
                           UnlockDoor commands (PIN validation is skipped
                           entirely since RequirePINforRemoteOperation
                           isn't set) and calls one of two plain C
                           functions — `emberAfPluginDoorLockOnDoorLockCommand()`/
                           `emberAfPluginDoorLockOnDoorUnlockCommand()` —
                           declared `__attribute__((weak))` with a
                           default `return false`, confirmed by reading
                           door-lock-server-callback.cpp directly;
                           door-lock-server.h's own comment above their
                           declaration literally says "should be
                           implemented by the server app". This file
                           provides the real (strong) definitions — the
                           documented extension point, not a workaround.
                           HandleRemoteLockOperation()'s own comment notes
                           "the app should trigger the lock state
                           change" — the framework does NOT update
                           LockState automatically after a successful
                           callback, so this file calls attribute::update()
                           itself. A second, separate linker requirement
                           was caught by an actual Docker build, not by
                           inspection: `emberAfDoorLockClusterInitCallback`
                           — unlike the two command callbacks above, this
                           one has a plain, non-weak prototype in
                           zzz_generated/app-common/app-common/
                           zap-generated/callback.h, so omitting it is a
                           hard `undefined reference` link error, not a
                           silent default. esp-matter's own
                           door_lock::function_list wires it in as the
                           cluster's init hook; the fix (confirmed against
                           the SDK's own examples/door_lock/main/lock/
                           door_lock_callbacks.cpp reference) is a
                           one-line body calling
                           `DoorLockServer::Instance().InitServer(endpoint)`
                           — documented as "a deprecated alias for
                           InitEndpoint with no delegate", exactly
                           matching this file's own null-delegate choice,
                           and required regardless since it registers
                           this endpoint's per-endpoint server state
                           (lockout timestamp, wrong-code attempt
                           counter) that the cluster's internal
                           command-handling logic expects to already
                           exist. `DOOR_LOCK_OUTPUT_TYPE` selects SERVO
                           (default — a hobby servo, e.g. SG90-class,
                           turning an existing thumb-turn deadbolt from
                           the inside, the same retrofit approach
                           countless DIY/ESPHome smart-lock projects use;
                           driven via ESP-IDF's driver/ledc.h, the same
                           LEDC PWM peripheral firmware/dimmable-light/
                           and firmware/color-light/ already use, at the
                           standard 50Hz/1-2ms hobby-servo signal) or
                           RELAY (an electric strike/solenoid, active-LOW,
                           matching firmware/outlet/'s own relay
                           convention). Optional `DOOR_LOCK_POSITION_GPIO`
                           (reed switch, off by default, `GPIO_NUM_NC`)
                           reads real bolt/latch position the same simple
                           digital HIGH/LOW technique
                           firmware/contact-sensor/ already uses; without
                           it, LockState is set OPTIMISTICALLY right
                           after actuating — a real, common pattern for
                           this class of cheap retrofit hardware (no
                           feedback = no way to know for certain), and
                           explicitly allowed by the spec's own framing.
                           `chip::app::Clusters::DoorLock::DlLockType` is
                           a real gotcha worth remembering for future
                           DoorLock-adjacent code: door-lock-server.h's
                           own top-level `using` declarations pull
                           `DlLockState`/`DlStatus`/several other DoorLock
                           enums unqualified into scope, but NOT
                           `DlLockType` — confirmed by an actual Docker
                           build failure (`'DlLockType' has not been
                           declared`) before fully qualifying it. Standard
                           quick-power-cycle factory reset, same as every
                           other device type here.
                           Build-verified in Docker across all 3
                           meaningful configs (servo/no-sensor, servo/
                           with-sensor, relay/no-sensor); not
                           hardware-tested (no servo/relay/reed-switch
                           hardware for this device type physically
                           available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/smoke-co-alarm/  Smoke/CO Alarm — fourteenth device type, and this
                           repo's first over the SmokeCoAlarm cluster
                           (life-safety alarm class, not a plain sensor
                           readout or actuator)
  main/app_main.cpp        endpoint::smoke_co_alarm::create() (Identify +
                           SmokeCoAlarm cluster) confirmed complete/directly
                           usable by reading esp_matter_endpoint.cpp's own
                           smoke_co_alarm::add(). Unlike firmware/door-lock/'s
                           DoorLock, SmokeCoAlarm IS a "code-driven" cluster
                           class in this SDK version (confirmed: a
                           smoke_co_alarm/ folder exists under
                           data_model_provider/clusters/, same signal
                           firmware/contact-sensor/'s BooleanState and
                           firmware/light-sensor/'s IlluminanceMeasurement
                           already used) — so SmokeState/COState/
                           TestInProgress/HardwareFaultAlert/etc. are all
                           written through SmokeCoAlarmCluster's own setter
                           API, looked up via the data model provider's
                           registry, the same update_contact_state()/
                           update_illuminance() pattern already established
                           elsewhere in this repo. SmokeCoAlarmCluster's own
                           setters already generate the right Matter events
                           internally (SmokeAlarm/COAlarm on transitioning to
                           Warning/Critical, AllClear on transitioning back
                           to Normal) — no manual event-generation code
                           needed. SetExpressedStateByPriority() computes
                           the cluster's single "headline state" attribute
                           from a fixed 9-entry priority order (life-safety
                           alarms first, then self-test, then secondary
                           conditions); interconnect/battery/end-of-service/
                           inoperative states are left at their defaults
                           (no interconnect wiring, no battery to monitor
                           on USB/PSU power, no service-life tracking for a
                           hobby MQ-series sensor) — same "smallest
                           reasonable next step" scoping as firmware/
                           door-lock/'s skipped PIN/credential/schedule
                           features. A real gap worth remembering for any
                           future *Request-style command cluster: a real
                           controller's SelfTestRequest command succeeds
                           entirely inside the SDK with no Delegate needed
                           (sets TestInProgress=true and
                           ExpressedState=Testing on its own), but nothing
                           *clears* TestInProgress afterwards unless the app
                           does it — confirmed by reading
                           SmokeCoAlarmCluster::HandleRemoteSelfTestRequest()
                           directly. sensor_task() polls GetTestInProgress()
                           each cycle and, after
                           SMOKE_CO_ALARM_SELF_TEST_DURATION_MS, calls
                           SetTestInProgress(false) and recomputes
                           ExpressedState, simulating a completed self-test.
                           `SENSOR_TYPE` selects SENSOR_MQ2_MQ7 (default —
                           both an MQ-2 smoke sensor and an MQ-7 CO sensor,
                           matching how real combination smoke+CO alarms are
                           sold as one product; enables both cluster
                           features), SENSOR_MQ2 (smoke only), or SENSOR_MQ7
                           (CO only). Both are the classic cheap analog
                           gas-sensor modules (heated tin-dioxide element,
                           resistance drops as gas concentration rises) —
                           deliberately NOT converted to a calibrated ppm
                           figure the way firmware/light-sensor/'s LDR is
                           converted to lux: MQ-series datasheets only
                           document ppm as a family of curves that shift
                           with each sensor's own load resistance/heater
                           voltage/burn-in state, and Matter's SmokeCoAlarm
                           cluster has no numeric concentration attribute
                           anyway (only the AlarmStateEnum Normal/Warning/
                           Critical tri-state) — so this is a plain
                           adjustable-millivolt-threshold classifier, meant
                           to be tuned per module/environment, not a
                           calibrated absolute reading. GPIO 34/35 (ADC1
                           channels 6/7 on classic ESP32) are the MQ2/MQ7
                           defaults — deliberately ADC1, not ADC2, same
                           reasoning as the light sensor's LDR. A 60s
                           SMOKE_CO_ALARM_WARMUP_MS window only suppresses
                           the initial power-on resistance-settling
                           transient from causing a false alarm — explicitly
                           documented as NOT a substitute for the real
                           24-48h burn-in MQ-series datasheets call for.
                           HardwareFaultAlert is set from a simple, module-
                           polarity-agnostic heuristic: several consecutive
                           readings pinned at the ADC's extreme raw values.
                           Standard quick-power-cycle factory reset.
                           Build-verified in Docker across
                           all 3 sensor configs (MQ2+MQ7, MQ2-only,
                           MQ7-only); not hardware-tested (no MQ-2/MQ-7
                           module physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/occupancy-sensor/  Occupancy Sensor — fifteenth device type. First
                           in this repo to use a device type whose cluster
                           has a real "at least one of N features" spec
                           requirement rather than every feature being
                           independently optional or a fixed mandatory set.
  main/app_main.cpp        `endpoint::occupancy_sensor::create()` confirmed
                           to be a complete, ready-to-use top-level helper
                           (Identify + OccupancySensing, no Groups) by
                           reading esp-matter's own generated
                           occupancy_sensor_device.cpp directly — matches
                           the CSA's own data_model/1.6/device_types/
                           OccupancySensor.xml exactly (both clusters
                           `<mandatoryConform/>`). Using the complete
                           top-level helper here — rather than hand-
                           assembling from lower-level free functions the
                           way firmware/color-light/ and
                           firmware/addressable-light/ originally did —
                           sidesteps that whole class of bug from the
                           start: `common::create<T>()`, the shared
                           template underneath every complete top-level
                           helper, always creates the endpoint's Descriptor
                           cluster automatically, which is exactly what
                           those two device types' hand-assembled
                           endpoints were found to be missing during this
                           same session's Apple Home hardware testing (see
                           "Open next steps" below for the full story).
                           OccupancySensing confirmed to be a "code-driven"
                           cluster class, same category as
                           firmware/contact-sensor/'s BooleanState,
                           firmware/temperature-sensor/'s
                           TemperatureMeasurement, and
                           firmware/smoke-co-alarm/'s SmokeCoAlarm (a real
                           `occupancy_sensing/` folder exists under
                           `data_model_provider/clusters/`, backed by
                           connectedhomeip's own `OccupancySensingCluster`
                           class) — so the Occupancy attribute is written
                           through `OccupancySensingCluster::SetOccupancy()`,
                           looked up via the data model provider's
                           registry, reusing firmware/contact-sensor/'s
                           `update_contact_state()` pattern almost verbatim
                           (one real, quickly-caught mistake along the way:
                           `OccupancySensingCluster` lives directly in
                           `chip::app::Clusters`, not nested under an
                           `OccupancySensing::` sub-namespace the way the
                           cluster's *attribute IDs* are — an actual Docker
                           build failure, not a guess, caught this).
                           OccupancySensing's eight sensing-modality
                           features (PIR/Ultrasonic/PhysicalContact/etc.)
                           form a single CSA "choice" group requiring at
                           least one, confirmed two ways: the cluster XML
                           marks every one of them `optionalConform
                           choice="a" more="true" min="1"` (the CSA's own
                           idiom for "at least 1 of this named choice set
                           is required"), and esp-matter's own generated
                           occupancy_sensing.cpp enforces it directly via a
                           `VALIDATE_FEATURES_AT_LEAST_ONE(...)` macro that
                           aborts cluster creation if none are set — this
                           firmware always sets PassiveInfrared, so it's
                           satisfied automatically, but worth remembering
                           for any future sensing modality added here.
                           OCCUPANCY_SENSOR_TYPE grew from PIR-only to
                           three chips soon after this device type first
                           shipped, once the user shared two real radar
                           modules (an unbranded RCWL-0516-class board and
                           a labeled HLK-LD2410) to add: PIR (default — a
                           cheap module, e.g. the ubiquitous HC-SR501 or
                           any of its many clones; same "smallest
                           reasonable next step" scoping this repo has
                           applied to every other device type's first
                           cut), RCWL-0516 (a cheap microwave Doppler
                           radar module), and HLK-LD2410 (a real 24GHz
                           mmWave human-presence radar with its own
                           configurable UART protocol — only its simple
                           OUT pin is used here, not that richer protocol,
                           same "smallest reasonable next step" scoping
                           again). All three turned out to share the exact
                           same electrical interface — a single, actively-
                           driven (no pull-up needed), active-HIGH,
                           3.3V-logic digital OUT pin — confirmed per chip
                           against real sourcing rather than assumed
                           identical just because they're all "motion
                           sensors": PIR against multiple independent
                           HC-SR501-class module documentation sources (no
                           single canonical datasheet exists — it's a
                           widely cloned hobbyist module, not one
                           manufacturer's own part, same "best available,
                           cross-checked" sourcing standard already used
                           for e.g. APA102/SM2335EGH), RCWL-0516 and
                           HLK-LD2410 each against their own widely-cited
                           pinout documentation (confirmed the HLK-LD2410's
                           OUT is 3.3V logic even though the module itself
                           needs a separate 5V supply — safe to wire
                           directly into an ESP32 GPIO with no level
                           shifting). Because all three share one GPIO
                           interface, the existing GPIO-read/debounce code
                           needed zero changes — only the
                           OccupancySensorType/OccupancySensorTypeBitmap/
                           FeatureMap values set at endpoint-creation time
                           differ per chip. A real spec gap surfaced while
                           wiring this up: the legacy 4-value
                           OccupancySensorTypeEnum / 3-bit
                           OccupancySensorTypeBitmap (both explicitly
                           `deprecateConform`, kept only for backward
                           compatibility — FeatureMap is the modern
                           authoritative source) have no Radar value at
                           all, confirmed by reading the cluster XML
                           directly — so both radar sensors fall back to
                           Ultrasonic as the closest available legacy
                           analog (another active-emission sensing
                           technology, unlike PIR's passive heat sensing),
                           documented in the code rather than left as an
                           unexplained magic number. PIR's own module
                           already has its own onboard analog "occupied
                           hold time" (an adjustable potentiometer,
                           commonly ~5s-300s); RCWL-0516 instead outputs a
                           *fixed* ~2s HIGH pulse per detected motion event
                           (no adjustable hold time — a real behavioral
                           difference from PIR); HLK-LD2410's own internal
                           presence algorithm (distance gating,
                           sensitivity, moving/static timeout) decides what
                           OUT does. None of that timing is reimplemented
                           in software for any of the three — this
                           firmware only reports whatever OUT is currently
                           doing; the same short software debounce every
                           other GPIO-input device type here uses exists
                           only to reject electrical noise, not to
                           implement any occupancy-hold behavior of its
                           own. Wizard integration needed zero new
                           mechanism — the exact same componentOptions/
                           componentDefineName picker firmware/
                           temperature-sensor/ and firmware/light-sensor/
                           already established, one more `COMPONENT_
                           LIBRARY` category (`occupancy-sensor`) with
                           three entries. Build-verified in Docker for all
                           three OCCUPANCY_SENSOR_TYPE values; only PIR is
                           hardware-tested (no RCWL-0516/HLK-LD2410 module
                           was physically on the bench for this specific
                           verification pass, beyond the photos used to
                           identify them). GPIO 4 default, same convention
                           firmware/contact-sensor/ and firmware/switch/
                           already use for their own single digital sensor
                           input. Build-verified in Docker and validated
                           end to end on real hardware: commissioned via
                           Apple Home (clean `CommissioningComplete`, no
                           `RemoveFabric` — confirmed via a live serial
                           log, same verification method used throughout
                           this session), then real PIR motion in front of
                           the sensor correctly flipped the Home app's
                           tile between "Aanwezig — geen beweging
                           gedetecteerd" and "Beweging gedetecteerd",
                           confirmed against the same live serial log
                           showing each debounced edge and the resulting
                           `ReportData` sent to the controller.
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/fan/              Fan — sixteenth device type, and this repo's
                           second genuine Delegate-based cluster after
                           firmware/window-covering/'s WindowCovering (see
                           below for how FanControl's own Delegate differs
                           in practice).
  main/app_main.cpp        `endpoint::fan::create()` (device type 0x002B)
                           confirmed complete/ready-to-use — Identify +
                           Groups + FanControl are the only
                           `<mandatoryConform/>` clusters per the CSA's own
                           data_model/1.6/device_types/Fan.xml (On/Off is
                           listed too but only `<optionalConform/>`,
                           deliberately not added — On/Off is folded into
                           PercentSetting instead, 0% = off, the same
                           "smallest reasonable next step" scoping this
                           repo applies to every other device type's first
                           cut) — and, like firmware/occupancy-sensor/'s
                           own top-level helper, confirmed to create the
                           endpoint's Descriptor cluster automatically via
                           `common::create<T>()`'s shared internal path,
                           sidestepping the class of bug firmware/
                           color-light/'s and firmware/addressable-light/'s
                           original hand-assembled endpoints hit (see
                           "Open next steps" below). FanControl confirmed
                           to be a "code-driven" cluster class (a real
                           `fan_control/` folder exists under
                           `data_model_provider/clusters/`) but, unlike
                           firmware/occupancy-sensor/'s OccupancySensing (a
                           plain registry-lookup setter is enough there),
                           FanControl is genuinely Delegate-based —
                           confirmed by reading `fan-control-delegate.h`,
                           which declares a real `FanControl::Delegate`
                           interface an app must subclass and register via
                           `SetDefaultDelegate()`, the same Delegate
                           pattern firmware/window-covering/'s
                           WindowCovering already established in this repo
                           (`HandleStep()` is pure virtual and must be
                           implemented even though the Step feature isn't
                           enabled — it just returns UnsupportedCommand;
                           `OnFanDriveStateChanged()` is where the real PWM
                           output actually gets driven). Confirmed directly
                           in FanControlCluster.cpp that writing FanMode
                           always cascades into PercentSetting too (Off->
                           0%, Low->33%, Medium->66%, High->100%), so this
                           file only ever needs to react to PercentSetting,
                           never FanMode separately. Two real, sequential
                           build failures were needed to land on the
                           correct integration pattern — not guessed from
                           reading headers alone: first a *compile* error
                           (fixed by switching from `fan-control-delegate.h`
                           to connectedhomeip's own
                           `app/clusters/fan-control-server/
                           CodegenIntegration.h`, which declares
                           `SetDefaultDelegate()` and a full
                           `Attributes::X::Get()/Set()` namespace); then,
                           even after that compiled cleanly, a genuine
                           *link* error
                           (`undefined reference to ...PercentCurrent::
                           Set`) — root-caused by actually reading
                           esp-matter's own
                           `data_model_provider/clusters/fan_control/
                           integration.cpp` (which is what esp-matter's
                           build substitutes for connectedhomeip's generic
                           `CodegenIntegration.cpp`, same "esp-matter has
                           its own integration file per code-driven
                           cluster" pattern occupancy-sensor's
                           OccupancySensing already established) and
                           confirming it only implements
                           `SetDefaultDelegate()` — not any of the
                           `Attributes::X::Set()` free functions the header
                           declares. Fixed with the same registry-lookup-
                           and-cast pattern firmware/contact-sensor/'s and
                           firmware/occupancy-sensor/'s own setters already
                           use instead (`SetPercentCurrent()` on the live
                           `FanControlCluster` instance, looked up via the
                           data model provider's registry) — worth
                           remembering as a fourth, genuinely distinct
                           "how do I write a code-driven cluster attribute
                           from app code" pattern in this repo now: (1)
                           plain registry-lookup setter (BooleanState/
                           OccupancySensing), (2) a Delegate whose own
                           reporting call happens to be a working generic
                           free-function proxy (WindowCovering's
                           `report_position()`), (3) a Delegate whose
                           reporting call needs the registry-lookup
                           fallback instead because esp-matter's own
                           integration.cpp is incomplete relative to what
                           connectedhomeip's generic header declares
                           (FanControl, here) — esp-matter's own per-
                           cluster integration.cpp must be read directly
                           each time, never assumed complete from
                           connectedhomeip's generic header alone. Every
                           one of FanControl's six optional features
                           (MultiSpeed/Auto/Rocking/Wind/Step/
                           AirflowDirection) is independently
                           `<optionalConform/>` — none implemented, driving
                           the fan purely through the cluster's own base-
                           mandatory PercentSetting/PercentCurrent (0-100%,
                           the natural fit for a PWM-driven DC fan with no
                           discrete speed taps to model). FanModeSequence
                           set to OffLowMedHigh (the only listed
                           FanModeSequenceEnum value offering three real
                           speed steps without requiring the Auto feature)
                           so a controller's own FanMode picker still
                           offers Low/Medium/High shortcuts alongside the
                           continuous percent slider PercentSetting always
                           provides. Output is real PWM via ESP-IDF's
                           `driver/ledc.h` — same LEDC peripheral/settings
                           firmware/dimmable-light/ already uses for LED
                           brightness, just driving a MOSFET or fan-speed-
                           controller board's PWM input instead, at 25kHz
                           (above the audible range, and the common PWM
                           frequency most 4-wire PC/DC fan speed inputs
                           expect) — PercentSetting's own 0-100 range is
                           used directly as the duty percentage, no
                           remapping needed. Standard quick-power-cycle
                           factory reset. Build-verified in Docker; not
                           hardware-tested (no PWM fan/MOSFET driver board
                           for this device type physically available when
                           written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/air-quality-sensor/  Air Quality Sensor — seventeenth device type,
                           and the first to combine a qualitative headline
                           state (AirQuality's own Good/Fair/.../
                           ExtremelyPoor enum) with real numeric readings
                           (concentration-measurement clusters) on the SAME
                           endpoint, confirmed as a legitimate spec
                           combination by reading the CSA's own
                           data_model/1.6/device_types/AirQualitySensor.xml
                           directly: Identify + AirQuality are the only
                           `<mandatoryConform/>` clusters, and every one of
                           ten concentration-measurement clusters (CO/CO2/
                           NO2/Ozone/PM1/PM2.5/PM10/Radon/Formaldehyde/
                           TVOC — even Temperature/Humidity) is
                           `<optionalConform/>` on that same endpoint, not a
                           separate one.
  main/app_main.cpp        `endpoint::air_quality_sensor::create()` (device
                           type 0x002C) confirmed complete/ready-to-use —
                           Identify + AirQuality, no Groups (correctly
                           matches the CSA XML, which lists none) — and,
                           like firmware/occupancy-sensor/'s and
                           firmware/fan/'s own top-level helpers, confirmed
                           to create the endpoint's Descriptor cluster
                           automatically via `common::create<T>()`.
                           CarbonDioxideConcentrationMeasurement and
                           TotalVolatileOrganicCompoundsConcentrationMeasurement
                           are then added onto that SAME endpoint afterwards
                           via their own `cluster::xxx_concentration_
                           measurement::create()` free functions — this
                           does NOT reintroduce the missing-Descriptor bug
                           (the complete helper already built the endpoint
                           correctly; more clusters are simply added onto
                           it afterwards), same "add extra clusters onto an
                           already-correct endpoint" pattern
                           firmware/thermostat/'s BINDING output type
                           already established. AirQuality confirmed to be
                           a "code-driven" cluster (a real `air_quality/`
                           folder exists under
                           `data_model_provider/clusters/`) — `SetAirQuality()`
                           is a plain method (not a Delegate), so this uses
                           the same registry-lookup-and-cast pattern
                           firmware/contact-sensor/'s and firmware/
                           occupancy-sensor/'s own setters already
                           establish. A real, documented esp-matter gap was
                           found and worked around by scoping down, not
                           patching around it: `air_quality::create()`
                           hardcodes `global::attribute::
                           create_feature_map(cluster, 0)` — unlike every
                           comparable "optional feature" cluster in this
                           repo (occupancy_sensing/smoke_co_alarm/
                           fan_control/concentration_measurement, all of
                           which thread a real `config->feature_flags`
                           through) `air_quality::config_t` doesn't even
                           declare a `feature_flags` field, so only the
                           base 3-state Good/Poor/Unknown scale is reachable
                           through this helper today — the finer Fair/
                           Moderate/VeryPoor/ExtremelyPoor states would need
                           an unverified FeatureMap override race against
                           `AirQualityCluster`'s own constructor-time
                           `BitFlags<Feature>` snapshot, judged worse than a
                           clean, correct 3-state scale for v1 (same
                           "smallest reasonable next step" scoping this
                           repo applies to every other device type's first
                           cut). Concentration-measurement clusters
                           confirmed NOT code-driven (no
                           `concentration_measurement/` folder under
                           `data_model_provider/clusters/`) — plain ember
                           attributes, written the same way as firmware/
                           door-lock/'s LockState (`esp_matter_nullable_float()`
                           + `attribute::update()`, no registry lookup
                           needed). `SENSOR_TYPE`-style
                           `AIR_QUALITY_SENSOR_TYPE` scaffold ships with one
                           sensor for v1 — CCS811, a real calibrated I2C
                           eCO2 (ppm)/eTVOC (ppb) gas sensor (not a raw-
                           analog MQ-series sensor the way firmware/
                           smoke-co-alarm/ uses) — chosen as the best fit
                           for Matter's numeric concentration clusters,
                           which need real calibrated units. Protocol
                           verified directly against ams's own "CCS811
                           Datasheet" (v1-06, 2019-Feb-07) and its
                           companion Programming Guide — both fetched as
                           PDFs and read via `pdftotext`, this repo's
                           established practice for primary-source
                           hardware protocol detail — not assumed from a
                           community library: I2C address 0x5A (ADDR pin
                           low) or 0x5B (high); nWAKE tied to GND per the
                           datasheet's own "simplest hardware" wiring
                           recommendation (no extra GPIO needed); boot
                           sequence (HW_ID=0x81 check, APP_VALID check,
                           APP_START write-with-no-data, MEAS_MODE=0x10 for
                           1 sample/sec — DRIVE_MODE bit position [6:4]
                           confirmed directly from the register's own bit
                           table); polling STATUS's DATA_READY/ERROR bits
                           and reading ALG_RESULT_DATA as a 5-byte
                           transaction, the datasheet's own documented
                           technique for polling without the nINT
                           interrupt pin; output ranges (eCO2 400-29206ppm,
                           eTVOC 0-32768ppb, both confirmed in-text) used
                           directly as Min/MaxMeasuredValue. The
                           datasheet's documented 20-minute conditioning/
                           run-in period is not implemented as a startup
                           delay — same "not a substitute for the real
                           hardware warm-up time" framing firmware/
                           smoke-co-alarm/ already uses for its own
                           MQ-series sensors' burn-in. AirQuality's
                           Good/Poor classification is a plain, adjustable
                           per-gas millivolt-style threshold (CO2 >=1000ppm
                           or TVOC >=660ppb -> Poor), not a spec-defined or
                           chip-calibrated mapping — same "adjustable
                           threshold, not a calibrated reading" precedent
                           firmware/smoke-co-alarm/'s own MQ classifier
                           already uses. Build-verified in Docker; not
                           hardware-tested (no CCS811 module physically
                           available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/water-leak-detector/  Water Leak Detector — eighteenth device
                           type, and the closest sibling to firmware/
                           contact-sensor/ in this repo: same BooleanState
                           cluster, same debounced-GPIO-input shape, but a
                           different device type (0x0043 vs. Contact
                           Sensor's 0x0015) and — critically — the
                           OPPOSITE semantic meaning of StateValue.
  main/app_main.cpp        `endpoint::water_leak_detector::create()`
                           confirmed structurally identical to
                           `contact_sensor::create()` by reading both
                           `add()` implementations side by side in
                           esp_matter_endpoint.cpp: Identify + BooleanState
                           + StateChange event, both via `common::
                           create<T>()` (auto-Descriptor) — matches the
                           CSA's own data_model/1.6/device_types/
                           WaterLeakDetector.xml exactly (Identify +
                           BooleanState mandatory, BooleanState's
                           ChangeEvent feature mandatory as of the device
                           type's own revision 2, BooleanStateConfiguration
                           optional and not implemented here — same
                           "smallest reasonable next step" scoping as
                           every other device type's first cut).
                           StateValue=true means "leak detected" here —
                           the OPPOSITE direction from contact-sensor's
                           true=closed — confirmed against real Matter
                           tooling rather than assumed: Espressif's own
                           `MatterWaterLeakDetector` Arduino-ESP32 class
                           exposes `setLeak(bool)` documented as
                           "leak detected" when true, matching Apple's own
                           HomeKit Leak Sensor characteristic direction
                           (0=No Leak, 1=Leak Detected) that Matter-to-
                           HomeKit bridges map this cluster onto. Sensor is
                           a cheap LM393-comparator "water sensor" probe
                           module (sold as FC-37/YL-83/generic "water
                           sensor module" — no single canonical datasheet,
                           a widely cloned design, same "best available,
                           cross-checked" sourcing standard as contact-
                           sensor's reed switch or occupancy-sensor's PIR
                           module) — confirmed via multiple independent
                           sources that its DO pin goes LOW when wet and
                           HIGH when dry, but unlike a passive reed switch
                           the module's own comparator actively drives DO
                           both ways, so no internal GPIO pull-up is used.
                           A real, previously undocumented esp-matter gap
                           was found (the same class as firmware/
                           air-quality-sensor/'s AirQuality FeatureMap
                           gap): `boolean_state::create()` also hardcodes
                           FeatureMap to 0 with no config field to
                           override it, so esp-matter's own
                           `water_leak_detector::add()` never actually
                           sets the ChangeEvent feature bit its own spec
                           (revision 2) requires. Unlike AirQuality's gap,
                           this one was judged safe to fix rather than
                           just document: confirmed by reading
                           `boolean_state::event::create_state_change()`
                           directly that the StateChange event fires
                           unconditionally with no feature-flag gate at
                           all, so the FeatureMap bit here is pure
                           advertised-conformance metadata, not something
                           gating real runtime behavior the way
                           `AirQualityCluster`'s constructor-time
                           `BitFlags<Feature>` snapshot does — fixed with a
                           direct `attribute::update()` on the
                           BooleanState cluster's FeatureMap attribute
                           between endpoint creation and
                           `esp_matter::start()`. Standard quick-power-
                           cycle factory reset. Build-verified in Docker;
                           not hardware-tested (no water sensor module
                           physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/air-purifier/    Air Purifier — nineteenth device type, and a
                           direct extension of firmware/fan/: same
                           FanControl cluster, same PWM output (reused
                           close to verbatim), plus HepaFilterMonitoring
                           and ActivatedCarbonFilterMonitoring on the same
                           endpoint — the two clusters that actually make
                           this an "Air Purifier" rather than a plain Fan.
  main/app_main.cpp        `endpoint::air_purifier::create()` confirmed
                           complete/ready-to-use (Identify + FanControl,
                           auto-Descriptor via `common::create<T>()`) by
                           reading esp_matter_endpoint.cpp's own
                           `air_purifier::add()` directly — matches the
                           CSA's own data_model/1.6/device_types/
                           AirPurifier.xml (Identify + FanControl
                           mandatory; Groups/On-Off/both filter-monitoring
                           clusters all optional). Groups/On-Off
                           deliberately not added, same scope decision
                           firmware/fan/ already made. FanControl itself
                           is reused near-verbatim from firmware/fan/ —
                           same Delegate, same registry-lookup-and-cast
                           for `SetPercentCurrent()`, same PercentSetting-
                           only scope, same 25kHz LEDC PWM output; see
                           that file's own header comment for the full
                           two-Docker-build-failure story behind that
                           pattern. HepaFilterMonitoring and
                           ActivatedCarbonFilterMonitoring are added onto
                           the SAME endpoint afterwards via their own
                           `cluster::hepa_filter_monitoring::create()`/
                           `cluster::activated_carbon_filter_monitoring::
                           create()` free functions — same "add extra
                           clusters onto an already-correct endpoint"
                           pattern firmware/thermostat/'s BINDING output
                           type and firmware/air-quality-sensor/'s
                           concentration-measurement clusters already
                           established. Confirmed by reading esp-matter's
                           own source that `resource_monitoring::create()`
                           (the shared template behind both filter
                           clusters) hardcodes FeatureMap to 0 just like
                           AirQuality/BooleanState's own gaps found in the
                           two device types before this one — but UNLIKE
                           those, esp-matter DOES expose a real, public
                           way to enable the Condition feature afterwards:
                           `cluster::resource_monitoring::feature::
                           condition::add(cluster, &config)`, a documented
                           API (not a raw FeatureMap override) that
                           properly read-modify-writes the feature bit via
                           `update_feature_map()` — still has to run
                           before `esp_matter::start()`, since
                           `ResourceMonitoringCluster` is confirmed
                           code-driven (a real `resource_monitor/` folder
                           exists under `data_model_provider/clusters/`)
                           and reads FeatureMap once at its own server-init
                           callback, same "constructor-time snapshot"
                           pattern AirQuality already established.
                           Warning/ReplacementProductList features and the
                           ResetCondition command are not implemented —
                           same "smallest reasonable next step" scoping as
                           every other device type's first cut. Updating
                           Condition/ChangeIndication at runtime uses a
                           real, ready-made free function esp-matter's own
                           `resource_monitor/integration.cpp` provides —
                           `ResourceMonitoring::GetClusterInstance(endpointId,
                           clusterId)` returning a `ResourceMonitoringCluster*`
                           with public `UpdateCondition()`/
                           `UpdateChangeIndication()` methods — rather than
                           this repo's usual registry-lookup-and-cast
                           pattern; worth remembering as a fifth, genuinely
                           distinct "how do I write a code-driven cluster
                           attribute from app code" pattern in this repo
                           now (after the plain registry-lookup setter, the
                           two Delegate variants, and the direct FeatureMap
                           `attribute::update()` override firmware/
                           water-leak-detector/ uses): a cluster-family-
                           specific convenience free function, when
                           esp-matter's own integration.cpp happens to
                           provide one. Filter life itself is a plain
                           time-based estimate, not a real sensor reading
                           (no differential-pressure or particulate-
                           accounting hardware assumed — same "smallest
                           reasonable next step" reasoning firmware/
                           window-covering/'s own time-based position
                           estimate already applies): while the fan is
                           actually running, elapsed operating seconds
                           accumulate in their own NVS namespace
                           (persisted every 60s while running, not on
                           every tick, to avoid flash wear) and Condition
                           is computed against each filter's own
                           configurable rated life in operating hours —
                           AIR_PURIFIER_HEPA_LIFE_HOURS (2000h) and
                           AIR_PURIFIER_CARBON_LIFE_HOURS (1000h),
                           commonly-cited commercial air-purifier figures
                           (carbon media saturates faster than HEPA media
                           in real products), both explicitly adjustable,
                           not calibrated measurements — same "adjustable
                           threshold, not a calibrated reading" precedent
                           firmware/smoke-co-alarm/'s own MQ classifier
                           already established. Standard quick-power-cycle
                           factory reset (which also clears the filter-life
                           counter, a reasonable side effect documented in
                           the code). Build-verified in Docker (two
                           real, sequential compile errors along the way —
                           a wrong `feature` namespace nesting order and a
                           missing `GetClusterInstance()` header include,
                           both fixed and confirmed by the actual build,
                           not guessed); not hardware-tested (no PWM fan/
                           MOSFET driver board physically available when
                           written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/valve/            Water Valve — twentieth device type, and this
                           repo's third genuine Delegate-based cluster
                           after firmware/window-covering/'s WindowCovering
                           and firmware/fan/'s (and firmware/air-purifier/'s)
                           FanControl — but the SDK does noticeably more of
                           the work here than either of those.
  main/app_main.cpp        `endpoint::water_valve::create()` (device type
                           0x0042 — Matter's own device type name is "Water
                           Valve") confirmed complete/ready-to-use —
                           Identify + ValveConfigurationAndControl, auto-
                           Descriptor via `common::create<T>()` — matches
                           the CSA's own data_model/1.6/device_types/
                           WaterValve.xml (Identify + Valve Configuration
                           and Control mandatory; Flow Measurement, both
                           server and client side, optional and not
                           implemented — same "smallest reasonable next
                           step" scoping as every other device type's
                           first cut). ValveConfigurationAndControl
                           confirmed code-driven (a real
                           `valve_configuration_and_control/` folder
                           exists under `data_model_provider/clusters/`),
                           Delegate-based like FanControl/WindowCovering —
                           but reading `ValveConfigurationAndControlCluster
                           .cpp` directly (not just the header) shows the
                           cluster owns substantially more internally than
                           either of those: Open/Close command handling,
                           OpenDuration/DefaultOpenDuration/
                           RemainingDuration/AutoCloseTime bookkeeping, and
                           the actual 1-second countdown timer that
                           auto-closes the valve when RemainingDuration
                           reaches 0 (including calling the app's own
                           `HandleCloseValve()` when that happens) are ALL
                           handled inside the cluster itself — this file
                           implements none of its own timing logic, unlike
                           WindowCovering's Delegate (which owns 100% of
                           its own movement timing). The Delegate genuinely
                           only needs to: actuate the relay in
                           `HandleOpenValve()`/`HandleCloseValve()`, and
                           optionally react to a once-a-second
                           `HandleRemainingDurationTick()` countdown
                           notification (logged here, not otherwise acted
                           on). One thing the cluster does NOT do
                           automatically, confirmed by reading
                           `HandleOpenCommand()`/`OpenValve()`/
                           `CloseValve()` end to end: it sets CurrentState
                           to Transitioning before calling the Delegate but
                           never automatically advances it to Open/Closed
                           afterwards — that's the app's own job via the
                           cluster's public `UpdateCurrentState()` method,
                           the same "the app should trigger the state
                           change" responsibility firmware/door-lock/'s own
                           header comment already documents for DoorLock's
                           LockState; CurrentState is set OPTIMISTICALLY
                           here too, same reasoning as door-lock's own
                           default (no position sensor assumed). A second
                           real, previously-undiscovered gap was found
                           while wiring up the Delegate itself: unlike
                           firmware/fan/'s FanControl or firmware/
                           air-purifier/'s ResourceMonitoring, esp-matter
                           ships NO public header declaring a
                           `SetDefaultDelegate()`-style free function for
                           this cluster at all (confirmed by checking — no
                           `integration.h` exists next to esp-matter's own
                           `valve_configuration_and_control/integration.cpp`,
                           unlike `resource_monitor/`'s) — worked around by
                           going straight through the cluster's own public
                           `SetDelegate()` method via the same registry-
                           lookup-and-cast pattern this file already uses
                           for `UpdateCurrentState()`, rather than assuming
                           a free function exists just because one did for
                           the two previous Delegate-based clusters. This
                           device type also directly prompted finding (see
                           "Open next steps" below) that firmware/fan/'s
                           and firmware/air-purifier/'s own
                           `FanControl::SetDefaultDelegate()` calls were
                           placed BEFORE `esp_matter::start()` and had been
                           silently no-oping the whole time — this file's
                           own delegate registration was written correctly
                           (after `start()`) from the start, once that
                           timing model was understood. No Level feature
                           (most cheap solenoid valve hardware is simple
                           on/off, not proportional — same "smallest
                           reasonable next step" reasoning as window-
                           covering's Lift-only scope). Boots closed
                           (CurrentState/TargetState = Closed), matching
                           every other device type's boot-to-known-safe-
                           state convention. Standard quick-power-cycle
                           factory reset. Build-verified in Docker; not
                           hardware-tested (no relay/solenoid-valve
                           hardware physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/pressure-sensor/  Pressure Sensor — twenty-first device type. The
                           simplest device type XML in this repo so far —
                           just Identify + one mandatory cluster, no
                           options at all.
  main/app_main.cpp        `endpoint::pressure_sensor::create()` (device
                           type 0x0305) confirmed complete/ready-to-use —
                           Identify + PressureMeasurement, auto-Descriptor
                           via `common::create<T>()` — matches the CSA's
                           own data_model/1.6/device_types/
                           PressureSensor.xml exactly: those two clusters
                           are the ONLY ones listed, both mandatory.
                           PressureMeasurement confirmed to be a
                           "code-driven" cluster class (a real
                           `pressure_measurement/` folder exists under
                           `data_model_provider/clusters/`), same category
                           as firmware/temperature-sensor/'s
                           TemperatureMeasurement — `SetMeasuredValue()`
                           via the registry, same pattern. MeasuredValue's
                           encoding (kPa, resolution 0.1 kPa) isn't
                           spelled out in Matter's own machine-readable
                           cluster XML (inherited from the Zigbee ZCL
                           cluster, whose own spec does document it) —
                           confirmed instead against Home Assistant's own
                           real, open-source Matter integration
                           (`sensor.py`'s PressureMeasurement discovery
                           schema divides the raw value by 10 and reports
                           kPa), rather than assumed. Conveniently 1 hPa =
                           0.1 kPa, so MeasuredValue is numerically
                           identical to hPa — the unit BMP280's own
                           datasheet already reports in, needing no
                           conversion at the call site.
                           `PRESSURE_SENSOR_TYPE` scaffold ships with one
                           sensor for v1 — BMP280, a real, extremely
                           common cheap I2C barometric pressure +
                           temperature sensor, chosen for the same "most
                           common hobbyist part" reasoning firmware/
                           air-quality-sensor/'s CCS811 was. Protocol
                           (I2C address, CHIP_ID check, calibration
                           register layout, ctrl_meas oversampling/mode
                           bits, and the compensation formula) verified
                           directly against Bosch's own official BMP280
                           datasheet (BST-BMP280-DS001, revision 1.26),
                           fetched as a PDF and read via `pdftotext` —
                           this repo's established practice for
                           primary-source hardware protocol detail.
                           A real, self-caught mistake during that
                           process worth remembering: the compensation
                           formula was initially written from memory
                           (recognized as the well-known 64-bit
                           fixed-point BMP280 algorithm) with a header
                           comment claiming it came from the datasheet's
                           appendix section 8.2 (the 32-bit fallback
                           variant) — re-checking the actual fetched PDF
                           text before finalizing caught that the code
                           and the citation didn't match; the code itself
                           turned out to be byte-for-byte correct against
                           the datasheet's own primary section 3.11.3
                           (64-bit, "best possible calculation accuracy"),
                           so only the citation needed fixing, but this is
                           exactly the kind of citation/implementation
                           mismatch this repo's "verify against the
                           actual fetched source, not memory" discipline
                           exists to catch — worth double-checking any
                           future compensation-formula code against the
                           literal fetched datasheet text one more time
                           before finalizing, even when the code being
                           written seems obviously familiar/correct from
                           training. Forced mode with Bosch's own
                           documented "Standard resolution" preset
                           (osrs_t=x1, osrs_p=x4, from the datasheet's own
                           Table 7 recommended-settings) — simpler and
                           lower-power than Normal mode's continuous
                           standby/IIR-filter configuration for a
                           slowly-changing quantity like barometric
                           pressure. Output range 300-1100 hPa (the
                           datasheet's own full-accuracy operating range)
                           used directly as Min/MaxMeasuredValue. Standard
                           quick-power-cycle factory reset. Build-verified
                           in Docker (clean first attempt); not
                           hardware-tested (no BMP280 module physically
                           available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/robot-vacuum/     Robotic Vacuum Cleaner (RVC) — twenty-second
                           device type, and this repo's biggest cluster-
                           integration surface so far: three separate
                           command-handling clusters on one endpoint
                           (RvcRunMode, RvcCleanMode, RvcOperationalState),
                           two different SDK integration mechanisms for
                           them, and a genuine (if intentionally simple)
                           mobile actuator — two independent drive motors —
                           instead of a single relay/PWM output.
  main/app_main.cpp        `endpoint::robotic_vacuum_cleaner::create()`
                           (device type 0x0074) confirmed complete/ready-
                           to-use — Identify + RvcRunMode + RvcOperational
                           State (with the mandatory OperationCompletion
                           event pre-registered), auto-Descriptor via
                           `common::create<T>()` — matches the CSA's own
                           data_model/1.6/device_types/
                           RoboticVacuumCleaner.xml exactly (those two
                           clusters plus Identify are the only
                           `<mandatoryConform/>` ones; RVC Clean Mode and
                           Service Area are both `<optionalConform/>`).
                           RvcCleanMode is added anyway, via
                           `cluster::rvc_clean_mode::create()` on the
                           already-correct endpoint afterwards — same "add
                           an extra cluster onto an endpoint the top-level
                           helper already built correctly" pattern
                           firmware/thermostat/'s BINDING output type and
                           firmware/air-quality-sensor/'s concentration-
                           measurement clusters already established —
                           since choosing vacuum vs. mop vs. both is core
                           to what makes a modern robot vacuum useful and
                           the integration work is identical to
                           RvcRunMode's. Service Area (per-room/per-zone
                           cleaning, its own supported-areas list/current-
                           area tracking/storage delegate) is NOT
                           implemented — a genuinely large feature needing
                           real room/map data this simple GPIO-level
                           firmware has no way to generate, same "smallest
                           reasonable next step" scoping every other
                           device type's first cut in this repo uses.

                           RvcRunMode/RvcCleanMode are both ModeBase-
                           derived and wired through esp-matter's own
                           `config->delegate` field, confirmed by reading
                           `esp_matter_cluster.cpp`'s own
                           `rvc_run_mode::create()`/`rvc_clean_mode::
                           create()`: they store the delegate pointer via
                           `set_delegate_and_init_callback()`, and the
                           actual `ModeBase::Instance` construction +
                           `Init()` happens later, automatically, during
                           `esp_matter::start()`'s own cluster-init pass
                           (`InitModeDelegate()` in
                           `esp_matter_delegate_callbacks.cpp`) — unlike
                           firmware/fan/'s `FanControl::
                           SetDefaultDelegate()`, there is no separate
                           call the app must remember to place after
                           `start()`; the delegate is simply part of
                           `config_t`, supplied once before
                           `endpoint::robotic_vacuum_cleaner::create()`.
                           Confirmed by reading `ModeBaseCluster.cpp`'s
                           own `Instance::HandleChangeToMode()` that the
                           SDK updates `CurrentMode` itself after a
                           successful delegate callback — neither
                           delegate here writes it directly, same "the SDK
                           does more of the work than the header alone
                           suggests" lesson firmware/valve/'s
                           ValveConfigurationAndControl already taught
                           this repo. Since esp-matter constructs each
                           `ModeBase::Instance` internally and never hands
                           this file a pointer to either one (and the data
                           model provider's registry lookup pattern used
                           elsewhere in this repo doesn't apply here — no
                           `mode_base/` folder exists under
                           `data_model_provider/clusters/`, confirmed by
                           checking), this file keeps two small `static`
                           globals (`g_current_run_mode`/
                           `g_current_clean_mode`) as its own single
                           source of truth for cross-delegate coordination
                           (RunMode needs to know CleanMode's current
                           selection when starting a clean; Resume needs
                           to know RunMode's own selection) — simple and
                           correct since these two delegates are the only
                           writers. Real mode/tag values (`RvcRunMode::
                           ModeTag::kIdle/kCleaning/kMapping`,
                           `RvcCleanMode::ModeTag::
                           kVacuum/kMop/kVacuumThenMop`) were confirmed
                           directly against connectedhomeip's own
                           generated `zzz_generated/app-common/clusters/
                           RvcRunMode(Clean)Mode/Enums.h`, and the mode-
                           option-list construction pattern is ported from
                           connectedhomeip's own real, working reference —
                           `examples/chef/common/chef-rvc-mode-
                           delegate.cpp` — same "port a real reference
                           rather than guess the integration shape"
                           precedent already used in this repo for
                           SM2335EGH/APA102/OpenTherm. This file's own
                           "only allowed to enter Mapping from Idle" and
                           "reject a clean-mode change while actively
                           cleaning" business rules are the same two rules
                           chef's own reference delegate encodes — reused
                           deliberately, since they reflect real
                           constraints, not an implementation detail.

                           RvcOperationalState is a different, deeper
                           case: esp-matter's own `config_t` for it is a
                           literally empty struct (`common::config_t`,
                           documented in-source as "Empty config for API
                           consistency") — confirmed by reading
                           `rvc_operational_state::create()` directly that
                           it only creates the base ember attributes, with
                           no delegate handling and no `Instance`
                           construction at all. A level deeper than
                           firmware/valve/'s own ValveConfigurationAndControl
                           gap (which was still registered with the data
                           model provider's registry automatically, just
                           missing a convenience free function):
                           RvcOperationalState is never registered
                           anywhere as a working command handler unless
                           this file builds one itself. Fixed the same way
                           connectedhomeip's own real reference app does
                           it (`examples/rvc-app/rvc-common/`, read
                           directly): construct a real, raw
                           `chip::app::Clusters::RvcOperationalState::
                           Instance` (declared in `app/clusters/
                           operational-state-server/
                           CodegenIntegration.h` — pre-wired with
                           `RvcOperationalState::Id` and the two RVC-
                           specific pause/resume-compatibility overrides
                           already implemented by connectedhomeip itself:
                           only `SeekingCharger` is pause-compatible, only
                           `Charging`/`Docked` are resume-compatible,
                           confirmed by reading `CodegenIntegration.cpp`
                           directly) with an app-supplied
                           `RvcOperationalState::Delegate` (declared in
                           `OperationalStateDelegate.h` — already gives
                           `HandleStartStateCallback`/
                           `HandleStopStateCallback` working dummy bodies,
                           since Start/Stop aren't part of RVC's own
                           command set). Constructed as a file-scope
                           `static` and `.Init()`'d in `app_main()`,
                           deliberately after `esp_matter::start()` — same
                           "register real command handling only once the
                           Matter server itself is actually running"
                           discipline this repo has now hit for FanControl
                           (firmware/fan/, firmware/air-purifier/) and
                           ValveConfigurationAndControl (firmware/valve/).
                           A second real SDK behavior confirmed by reading
                           `CodegenIntegration.cpp`'s own
                           `HandlePauseState()`/`HandleResumeState()`/
                           `HandleGoHomeCommand()` directly and worth
                           remembering for any future OperationalState-
                           derived cluster: unlike ModeBase, NONE of them
                           update the `OperationalState` attribute
                           automatically after a successful delegate
                           callback — every callback in this file calls
                           the Instance's own `SetOperationalState()`
                           itself, exactly matching the real reference
                           app's own `rvc-device.cpp`. The mandatory
                           `OperationCompletion` event fires via the
                           Instance's own real `OnOperationCompletionDetected()`
                           method whenever RvcRunMode transitions from
                           Cleaning/Mapping back to Idle — confirmed by
                           reading that method directly that it only
                           calls connectedhomeip's own `LogEvent()`,
                           independent of the ember-attribute-store event
                           descriptor `endpoint::robotic_vacuum_cleaner::
                           add()` sets up for spec-conformance metadata —
                           a real, working event, not a placeholder.

                           Hardware scope is deliberately simple and
                           explicitly honest about what it doesn't do: two
                           independent drive motors (left/right wheel) via
                           a dual H-bridge driver module (L298N/
                           TB6612FNG-class), FWD/REV GPIO pair per motor
                           (mutually exclusive by construction, same
                           pattern firmware/window-covering/'s single UP/
                           DOWN motor uses, doubled here), fixed speed
                           only (no PWM enable line). A vacuum-motor
                           output (MOSFET/relay, active-HIGH) and a mop-
                           pump output (active-LOW relay, matching
                           firmware/valve/'s own relay-polarity
                           convention) are switched together according to
                           whichever RvcCleanMode is selected. An optional
                           dock-contact sensor input (default
                           `GPIO_NUM_NC`, off — same opt-in-GPIO
                           convention firmware/door-lock/'s position
                           sensor uses) reads a simple digital HIGH-when-
                           docked signal. Explicitly, deliberately NOT
                           implemented: any actual navigation, obstacle
                           avoidance, or return-to-dock path-finding —
                           "Cleaning"/"Mapping" both just drive both
                           wheels forward at a fixed speed, since there is
                           no camera/LIDAR/bump/cliff sensor assumed and
                           therefore nothing to steer with. This is the
                           same category of honest scope cut as firmware/
                           window-covering/'s own documented position-
                           drift limitation, just larger in scope, since
                           full autonomous navigation is an entire
                           separate engineering discipline (SLAM, sensor
                           fusion, path planning) rather than a hardware-
                           driver detail. `GoHome` reflects this honestly:
                           it stops the drive motors and reports
                           `SeekingCharger` rather than pretending to
                           navigate; with a dock-contact sensor wired,
                           reaching the dock (by whatever means) is
                           detected and reported as `Charging`; without
                           one, `GoHome` optimistically reports `Docked`
                           directly — the same "no feedback sensor =
                           optimistic best-effort report" precedent
                           firmware/door-lock/'s LockState and firmware/
                           valve/'s CurrentState already establish.
                           Standard quick-power-cycle factory reset.
                           Build-verified in Docker (two real, sequential
                           compile errors were caught and fixed along the
                           way, not guessed: a `chip::app::Clusters::
                           detail` vs. `chip::detail` namespace ambiguity
                           from an initial blanket `using namespace
                           chip;`, fixed by narrowing to `using namespace
                           chip::literals;` for just the `_span` string-
                           literal operator and fully qualifying every
                           other `chip::`/`chip::app::` name instead); not
                           hardware-tested (no robot chassis/motor-driver
                           hardware physically available when written).
                           Integrated into `tools/product-wizard/` in a
                           follow-up pass (initially deferred — see "Open
                           next steps" below for why): 6 required GPIO
                           fields (`driver` + a fixed 5-entry
                           `extraButtons` array, reusing firmware/
                           color-light/'s "fixed set, not a variable
                           count" mode of that mechanism unchanged, zero
                           new code) plus 1 optional field, a new
                           `dockSensor` — a deliberate third parallel copy
                           of `statusLed`/`positionSensor`'s identical
                           single-GPIO checkbox-gated shape (not a reuse
                           of either literal field, since both carry
                           hardcoded, device-type-specific copy that would
                           misdescribe a charging-dock contact), touching
                           every site `positionSensor` itself needed when
                           it was added for door-lock: the enable-check
                           helper, the render block, validation, the sed
                           command, three separate summary-row renderers,
                           device-type-switch reset, and DOM event
                           listeners. Verified with the same Node.js
                           sandboxed regression-check pattern this
                           wizard's own history already establishes
                           (device-type lookup; `renderConfigureDevice`
                           output containing every field's own label/
                           checkbox; `isProductComplete` before any prior
                           render — the same regression class
                           `hasVariableButtonCount` and `positionSensor`
                           were each checked against when added — and
                           after, with the dock sensor both off and on;
                           the exact generated sed commands for all 7
                           `#define`s) — then run for real: the generated
                           sed commands were executed against a copy of
                           the actual `app_main.cpp` and diffed against
                           the original, confirming a byte-for-byte match
                           except the one line deliberately changed.
                           No headless-Chromium screenshot pass this time
                           (none was available in this environment) —
                           the sandboxed HTML-content assertions plus the
                           real sed-and-diff check were judged sufficient
                           given the change reuses two already-screenshot-
                           verified mechanisms (`extraButtons`,
                           `statusLed`/`positionSensor`) rather than
                           introducing a new visual shape.
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/extractor-hood/  Extractor Hood — twenty-third device type, and the
                           closest sibling to firmware/air-purifier/ in this
                           repo: the same FanControl Delegate +
                           HepaFilterMonitoring/
                           ActivatedCarbonFilterMonitoring integration,
                           reused almost verbatim, for a kitchen range hood
                           instead of a room air purifier.
  main/app_main.cpp        `endpoint::extractor_hood::create()` (device
                           type 0x007A) confirmed complete/ready-to-use —
                           FanControl only, via `common::create<T>()`
                           (auto-Descriptor) — matches the CSA's own
                           data_model/1.6/device_types/ExtractorHood.xml
                           exactly: FanControl is the ONLY
                           `<mandatoryConform/>` cluster — Identify, HEPA
                           Filter Monitoring, and Activated Carbon Filter
                           Monitoring are all `<optionalConform/>`. First
                           device type in this repo where Identify itself
                           is optional per spec rather than mandatory
                           (confirmed directly in the XML) —
                           `extractor_hood::add()` correspondingly does
                           NOT call `identify::create()` at all, unlike
                           every other top-level helper used so far. Added
                           anyway, via `cluster::identify::create()` on the
                           endpoint afterwards — every other device type
                           here ships an Identify LED, and nothing in the
                           spec disallows one here. The XML also
                           explicitly `<disallowConform/>`s three
                           FanControl features on this device type —
                           Rocking (RCK), Wind (WND), and AirflowDirection
                           (DIR) — zero code impact (neither firmware/fan/
                           nor firmware/air-purifier/ implement any of
                           those three anyway), but a real, checked spec
                           constraint worth recording rather than a
                           coincidence. FanControl itself, the Delegate-
                           registration-after-`start()` pattern, the LEDC
                           PWM output, and the filter-monitoring clusters'
                           `feature::condition::add()` + `ResourceMonitoring
                           ::GetClusterInstance()` integration are all
                           reused near-verbatim from firmware/air-purifier/
                           — see that file's own header comment for the
                           full detail on every one of those, including
                           the real Docker build failures and ordering
                           bugs that established each pattern in the first
                           place. The one real, deliberate difference:
                           "HEPA Filter Monitoring" (cluster 0x0071) is
                           repurposed here to represent the hood's actual
                           grease filter (typically a washable metal mesh/
                           baffle filter, not literally HEPA media) since
                           Matter has no dedicated grease-filter cluster —
                           confirmed by reading the device type XML
                           directly, which offers no alternative. Both
                           filter clusters' default life figures were
                           adjusted for what a range hood's own filters
                           actually are rather than reused from air-
                           purifier's air-purifier-specific figures:
                           EXTRACTOR_HOOD_GREASE_FILTER_LIFE_HOURS (100h)
                           and EXTRACTOR_HOOD_CARBON_FILTER_LIFE_HOURS
                           (200h) — same "adjustable threshold, not a
                           calibrated reading" precedent firmware/
                           smoke-co-alarm/'s and firmware/air-purifier/'s
                           own filter-life figures already establish.
                           Build-verified in Docker (clean first attempt);
                           not hardware-tested (no PWM fan/MOSFET driver
                           board physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/water-heater/    Water Heater — twenty-fourth device type, and the
                           first to combine a plain-ember-attribute
                           Thermostat cluster, a ModeBase-derived Mode
                           cluster, and a Delegate-driven command cluster
                           with its own events, all on one endpoint — plus
                           a genuinely new cluster (WaterHeaterManagement)
                           this repo hadn't touched before.
  main/app_main.cpp        `endpoint::water_heater::create()` (device type
                           0x050F) confirmed complete/ready-to-use —
                           Thermostat[Heat] + WaterHeaterManagement +
                           WaterHeaterMode, auto-Descriptor via
                           `common::create<T>()` — matches the CSA's own
                           data_model/1.6/device_types/WaterHeater.xml
                           exactly (all three mandatory; Identify only
                           `<optionalConform/>`, same spec shape firmware/
                           extractor-hood/ already hit, added onto the
                           endpoint afterward the same way). An optional
                           composed Electrical Sensor device type is
                           listed too — not implemented, same "smallest
                           reasonable next step" scope cut as every other
                           device type's own optional extras; firmware/
                           outlet/ already has that exact two-cluster
                           pattern built if it's ever wanted here.
                           ControlSequenceOfOperation is HeatingOnly — a
                           water heater physically can't cool, so unlike
                           firmware/thermostat/'s own Heat+Cool scope
                           there's no ambiguity to scope down at all.
                           WaterHeaterMode is wired through esp-matter's
                           own `config->delegate` field exactly like
                           firmware/robot-vacuum/'s RvcRunMode/RvcCleanMode
                           — confirmed by reading `esp_matter_cluster.cpp`'s
                           own `water_heater_mode::create()` directly, same
                           automatic `WaterHeaterModeDelegateInitCB` ->
                           `InitModeDelegate()` construction, no ordering
                           awareness needed. Real mode/tag values
                           (`WaterHeaterMode::ModeTag::kOff/kManual/kTimed`)
                           confirmed against connectedhomeip's own
                           generated Enums.h. "Timed" behaves identically
                           to "Manual" — no RTC-driven schedule
                           implemented, same honest scope cut as
                           firmware/robot-vacuum/'s "Mapping" mode having
                           no real navigation. WaterHeaterManagement is
                           ALSO wired through `config->delegate` the same
                           automatic way (confirmed directly) — but
                           confirmed by reading `Delegate.h` that every one
                           of its attributes (HeaterTypes/HeatDemand/
                           TankVolume/EstimatedHeatRequired/TankPercentage/
                           BoostState) is delegate-driven via a pure-
                           virtual getter, read live on every request,
                           despite esp-matter's own `config_t` exposing
                           `heater_types`/`heat_demand`/`boost_state`
                           fields that get seeded into now-irrelevant
                           ember attributes — same "code-driven cluster
                           shadows the plain ember store" pattern this
                           repo has hit repeatedly (RvcOperationalState,
                           ModeBase, ResourceMonitoring).
                           GetTankVolume()/GetEstimatedHeatRequired()/
                           GetTankPercentage() all return trivial
                           placeholders (0) — confirmed SAFE, not just
                           assumed, by reading
                           `WaterHeaterManagementCluster.cpp`'s own
                           `Attributes()` method: it only advertises those
                           three at all when the EnergyManagement/
                           TankPercent feature bits are set, which this
                           file leaves off, so a real controller never has
                           an attribute path that would reach those
                           getters. A real, previously-undocumented gap
                           was found while researching this: connectedhomeip's
                           own reference example
                           (`examples/water-heater-app/`) contains a
                           literal `// TODO: Implement Thermostat Cluster
                           temperature handling. It's mandatory to be spec
                           conformant.` comment — confirming there is no
                           single prescribed relationship between
                           Thermostat's own SystemMode and WaterHeaterMode's
                           Off/Manual/Timed even from the SDK authors
                           themselves. This file's own deliberate,
                           documented choice: heating is enabled only when
                           BOTH `WaterHeaterMode != Off` AND `Thermostat
                           SystemMode == Heat` — either cluster saying Off
                           turns the heater off. Both boot to Off, matching
                           every other device type's boot-to-known-safe-
                           state convention. `Boost`/`CancelBoost` are
                           handled entirely in this file (confirmed the
                           cluster itself has no countdown/state-machine
                           logic of its own, unlike e.g. firmware/valve/'s
                           ValveConfigurationAndControl) — the countdown,
                           the hysteresis control loop, and the DS18B20
                           sensor read all run together in one periodic
                           FreeRTOS task, calling straight into the
                           delegate's own state/event-generation methods
                           with no explicit Matter stack lock, the same
                           lock-free precedent firmware/air-purifier/'s
                           `filter_life_task` already established (re-
                           confirmed by re-reading that file — no
                           `lock::ScopedChipStackLock` there either).
                           `OneShot` is honestly implemented — this device
                           has a real local temperature reading to check
                           it against, so the task ends the boost itself,
                           generating a real `BoostEnded` event, once the
                           target is reached. `EmergencyBoost` is accepted
                           and logged but has no second heat source to
                           enable in this single-relay v1 design — a
                           documented, honest scope cut, same category as
                           firmware/robot-vacuum/'s "Mapping" mode. Single
                           relay (active-LOW, matching firmware/valve/'s
                           and firmware/door-lock/'s own convention) drives
                           a contactor in series with an immersion heating
                           element's own thermostat — the classic DIY
                           smart-electric-water-heater retrofit, not a gas
                           boiler ignition interface (a meaningfully
                           different, less hobbyist-safe problem).
                           `GetHeaterTypes()` is correspondingly fixed to
                           `kImmersionElement1` (confirmed against the
                           cluster's own `WaterHeaterHeatSourceBitmap`
                           enum, which also lists ImmersionElement2/
                           HeatPump/Boiler/Other for installation types
                           not implemented here); `GetHeatDemand()` mirrors
                           the relay's real, current state.
                           `WATER_HEATER_SENSOR_GPIO` reuses firmware/
                           thermostat/'s own DS18B20 driver verbatim —
                           deliberately NOT the full 7-chip `SENSOR_TYPE`
                           library that file offers for room-air
                           temperature: DS18B20 is specifically sold in a
                           waterproof stainless-steel probe variant widely
                           used for exactly this (tank/aquarium/brewing)
                           purpose, while the other six options are all
                           bare room-air sensors with no waterproof form
                           factor — offering them here would suggest
                           submerging hardware never designed for it.
                           `WATER_HEATER_HYSTERESIS_CENTIDEGREES` (0.5 degC)
                           is deliberately wider than firmware/thermostat/'s
                           own 0.3 degC default — a water tank's thermal
                           mass responds far more slowly than room air, so
                           a tighter band would just cause needless relay
                           chatter with no real benefit. Standard quick-
                           power-cycle factory reset. Two real, sequential
                           compile errors were caught and fixed by an
                           actual Docker build, not guessed: the same
                           `_span` literal-operator scoping fix firmware/
                           robot-vacuum/ already needed (`using namespace
                           chip::literals;`, not a blanket `using namespace
                           chip;`), and `WaterHeaterHeatSourceBitmap`/
                           `Energy_mWh` both needing their real fully-
                           qualified names (`WaterHeaterManagement::
                           WaterHeaterHeatSourceBitmap`, `chip::Energy_mWh`
                           — NOT `chip::app::Clusters::Globals::Energy_mWh`,
                           an initially-guessed wrong namespace path).
                           Build-verified in Docker; not hardware-tested
                           (no relay/DS18B20 probe hardware physically
                           available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/evse/            EVSE (electric vehicle charger controller) —
                           twenty-fifth device type, and this repo's first
                           over the Energy EVSE cluster family — ships with
                           a prominent safety note (see below) since this
                           is the first device type where getting the real-
                           world framing wrong could be genuinely unsafe,
                           not just inaccurate.
  main/app_main.cpp        `endpoint::energy_evse::create()` (device type
                           0x050C) confirmed complete/ready-to-use —
                           EnergyEvse + EnergyEvseMode + DeviceEnergy
                           Management, auto-Descriptor via `common::
                           create<T>()` — by reading esp_matter_endpoint.cpp's
                           own `energy_evse::add()` directly. A real,
                           checked discrepancy was found doing this:
                           the CSA's own data_model/1.6/device_types/
                           EVSE.xml lists only Identify + Temperature
                           Measurement (both optional) and a composed
                           Electrical Sensor device type as this device
                           type's OTHER clusters — DeviceEnergyManagement
                           isn't mentioned anywhere in that XML at all, yet
                           esp-matter's own top-level helper adds it
                           unconditionally. Kept as esp-matter ships it
                           (left at its default — `delegate = nullptr`,
                           `feature_flags = 0` — confirmed by reading
                           `esp_matter_cluster.cpp`'s own `device_energy_
                           management::create()` that this creates only
                           the cluster's base ember attributes with no
                           delegate wiring at all, the same "nullptr
                           delegate is a no-op" behavior every other
                           `config->delegate`-driven cluster in this repo
                           already has). Identify is added onto the
                           endpoint afterward — same "optionalConform, not
                           auto-wired" pattern firmware/extractor-hood/
                           and firmware/water-heater/ already hit.
                           Temperature Measurement and the composed
                           Electrical Sensor device type are both left
                           out — same "smallest reasonable next step"
                           scope cut every other device type's optional
                           extras get.

                           EnergyEvseMode is a fourth ModeBase-derived
                           cluster this repo has built (after RvcRunMode/
                           RvcCleanMode/WaterHeaterMode), identical
                           `config->delegate` automatic-construction
                           integration. Manual/TimeOfUse/SolarCharging are
                           all real, selectable modes that behave
                           identically to Manual — no tariff schedule or
                           solar-surplus tracking implemented, same
                           honest scope cut as WaterHeaterMode's own
                           "Timed" mode. `kV2X` mode tag exists in the
                           spec but is never offered — no V2X hardware.

                           EnergyEvse itself is this repo's biggest single
                           Delegate interface so far (confirmed by reading
                           `Delegate.h` directly — roughly 20 pure virtual
                           methods, including per-attribute `OnXChanged`
                           reactive callbacks with no default bodies at
                           all, unlike RvcOperationalState's Start/Stop
                           dummy defaults). Confirmed by reading
                           `EnergyEvseCluster.cpp`'s own `HandleDisable()`/
                           `HandleEnableCharging()` directly that the
                           cluster only does a plain min/max-current
                           sanity check before forwarding straight to the
                           Delegate — no State/SupplyState transition of
                           its own, same "cluster validates the command
                           shape, the app decides the actual state" split
                           firmware/water-heater/'s WaterHeaterManagement
                           already established. Unlike that cluster's
                           Delegate, `EnergyEvse::Delegate` has NO
                           `SetInstance()`/`GetInstance()` back-pointer at
                           all (confirmed directly) — reporting State back
                           uses the registry-lookup-and-cast pattern
                           firmware/valve/'s and firmware/fan/'s own
                           setters already established instead.
                           `ChargingPreferences` (`PREF`) is confirmed
                           `<mandatoryConform/>` directly in the cluster
                           XML's own `<features>` block — genuinely
                           mandatory, not an optional extra — so
                           SetTargets/GetTargets/ClearTargets/LoadTargets
                           are real, working, bounded in-memory storage (a
                           controller can round-trip real schedule data),
                           but honestly, no automatic scheduler in this
                           file acts on stored targets (no real-time-of-
                           day clock, and no pilot-signal current
                           negotiation to act on a target with anyway),
                           same honesty precedent as EnergyEvseMode's own
                           inert extra modes above. `EnableDischarging`
                           always rejects (no V2X); `StartDiagnostics`
                           accepts and logs but runs no real self-test,
                           same simulated-self-test precedent firmware/
                           smoke-co-alarm/'s own SelfTestRequest already
                           established. `FaultState` is always `NoError`
                           — no ground-fault/over-current/contact-welding
                           detection hardware assumed. `State::
                           PluggedInDemand` is never reported — telling it
                           apart from merely-connected needs real Control-
                           Pilot state-B/state-C detection this firmware
                           doesn't have.

                           *** This device type ships with a prominent
                           safety note at the top of app_main.cpp, worth
                           repeating here: this firmware does NOT
                           implement the real SAE J1772/IEC 61851 Control
                           Pilot protocol (the PWM signal + voltage-level
                           state machine a real EV charger uses to
                           negotiate current with the vehicle and detect
                           plug/fault states) — that's real, safety-
                           relevant automotive-grade engineering, well
                           outside what this repo's "read the datasheet,
                           drive the GPIO" style should be trusted to
                           implement for something that switches vehicle
                           charging current. The single relay
                           (`EVSE_RELAY_GPIO`, active-LOW) is designed to
                           drive an ALREADY-APPROVED EVSE unit's own low-
                           voltage enable/authorize dry-contact input —
                           the same simple external-control interlock many
                           commercial EVSEs already expose — NOT to switch
                           AC mains or vehicle charging current directly.
                           Same "gate an existing appliance's own control
                           input" framing as firmware/thermostat/'s RELAY
                           output (a boiler's call-for-heat input, not its
                           gas valve). *** An optional plug-detect input
                           (`EVSE_PLUG_DETECT_GPIO`, off by default) reads
                           a real EVSE unit's own "vehicle connected"
                           status output if it has one — same opt-in-GPIO,
                           optimistic-when-absent convention firmware/
                           door-lock/'s position sensor and firmware/
                           robot-vacuum/'s dock sensor already use.
                           `EVSE_CIRCUIT_CAPACITY_MA`/`EVSE_MIN_CHARGE_
                           CURRENT_MA`/`EVSE_MAX_CHARGE_CURRENT_MA` are
                           plain informational #defines (6000/32000 mA
                           defaults — 6A is IEC 61851's own documented
                           minimum EV charging current, 32A a common
                           single-phase EU home circuit rating), never
                           communicated to a vehicle via any pilot signal
                           since none is implemented. Standard quick-
                           power-cycle factory reset. Build-verified in
                           Docker; not hardware-tested (no EVSE hardware
                           with a real external-enable input physically
                           available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/generic-switch/  Generic Switch — twenty-sixth device type, and
                           this repo's first "smart button" accessory: a
                           plain momentary pushbutton that fires real
                           InitialPress/LongPress/ShortRelease/LongRelease/
                           MultiPressOngoing/MultiPressComplete events for
                           automations, rather than sending a command to a
                           bound target the way firmware/switch/'s own
                           On/Off Switch does.
  main/app_main.cpp        `endpoint::generic_switch::create()` (device
                           type 0x000F) confirmed complete/ready-to-use —
                           Identify + Switch, via `common::create<T>()`
                           (auto-Descriptor) — matches the CSA's own
                           data_model/1.6/device_types/GenericSwitch.xml
                           exactly (those two are the ONLY clusters
                           listed, both mandatory). Unlike firmware/
                           extractor-hood/'s, firmware/water-heater/'s,
                           and firmware/evse/'s own endpoint helpers,
                           Identify IS wired in automatically here — the
                           first of several recent device types that
                           hasn't needed a manual `cluster::identify::
                           create()` call. Confirmed by reading
                           `esp_matter_cluster.cpp`'s own `switch_cluster::
                           create()` directly that Switch is a plain
                           ember-attribute cluster (NumberOfPositions/
                           CurrentPosition are ordinary attributes, no
                           `config->delegate` field exists at all) — but
                           the six press/release/multi-press events are
                           generated through a real, registry-registered
                           `chip::app::Clusters::SwitchCluster` (a
                           `DefaultServerCluster`, same category as
                           `FanControlCluster`/`ValveConfigurationAndControlCluster`
                           — and, unlike firmware/evse/'s
                           `EnergyEvse::EnergyEvseCluster`, this one
                           resolves directly under `chip::app::Clusters`
                           with no extra sub-namespace qualification,
                           confirmed by reading `SwitchCluster.h`'s own
                           namespace block), reached via the same
                           registry-lookup-and-cast pattern. None of its
                           `OnXxx()` event methods touch CurrentPosition
                           at all (confirmed by reading `SwitchCluster.cpp`
                           directly — each only calls `GenerateEvent()`),
                           so this file calls `SetCurrentPosition()`
                           separately alongside each event.

                           Features: `momentary_switch` (MS) +
                           `momentary_switch_release` (MSR) +
                           `momentary_switch_long_press` (MSL) +
                           `momentary_switch_multi_press` (MSM) — a
                           complete single/double/triple/quadruple-click-
                           plus-long-press button, the same capability
                           set real commercial "smart button" accessories
                           expose. `action_switch` (AS, a scene-selector-
                           style switch) and `latching_switch` (a toggle/
                           rocker) are both out of scope — the cluster's
                           own `VALIDATE_FEATURES_EXACT_ONE` check
                           (confirmed directly) makes momentary and
                           latching mutually exclusive anyway, and a
                           plain multi-click button is the more directly
                           automatable hobbyist use case. A real,
                           previously-unnoticed namespace gotcha was
                           caught by an actual Docker build: these
                           features live under `cluster::switch_cluster::
                           feature::momentary_switch::get_id()` etc. —
                           NOT a flat `cluster::feature::` namespace
                           shared across every cluster, confirmed by
                           reading `esp_matter_feature_impl.h` directly
                           (feature is nested inside EACH cluster's own
                           namespace, e.g. `descriptor::feature::
                           tag_list`, `zone_management::feature::
                           focus_zones` — a pattern worth remembering for
                           any future `feature::xxx::add()` call in this
                           repo).

                           The press-timing state machine itself (debounce,
                           long-press threshold, multi-press windowing) is
                           real engineering this repo had to do itself —
                           checked directly that no example in the esp-
                           matter/connectedhomeip SDK (including
                           `examples/chef/common/clusters/switch/`)
                           implements a real GPIO-to-press-timing driver;
                           chef's own `SwitchManager.cpp` is a test-event-
                           injection harness, not something reading a real
                           button, and the cluster's own
                           `TestSwitchCluster.cpp` only exercises each
                           `OnXxx()` call in isolation. This file's own
                           state machine (`switch_task`, a periodic poll
                           rather than firmware/switch/'s ISR+queue shape,
                           since this one also needs to track ongoing
                           *duration*) uses the same industry-standard
                           technique virtually every DIY multi-click
                           button library uses: a 1000ms long-press
                           threshold (the common consumer-smart-button
                           convention) and a 400ms post-release multi-
                           press window (the common double-click timing
                           window). Both `MultiPressOngoing`'s field
                           constraint (`CurrentNumberOfPressesCounted`
                           between 2 and MultiPressMax) and
                           `MultiPressComplete`'s (only a `max`
                           constraint, no `min: 2`) were checked directly
                           in the cluster XML before writing this logic —
                           confirming MultiPressOngoing never fires for a
                           lone single click, but MultiPressComplete
                           correctly DOES fire even after one (reporting
                           count=1), the real, spec-grounded reason a
                           plain single click still produces a
                           MultiPressComplete event here, not just an
                           unpaired ShortRelease. A long press doesn't
                           chain into multi-press counting afterward —
                           matches how every real multi-click button
                           product behaves. Standard quick-power-cycle
                           factory reset. Build-verified in Docker (two
                           real, sequential compile errors caught and
                           fixed: a missing `#include
                           <app/clusters/switch-server/switch-server.h>`
                           for `SwitchCluster` itself, and the
                           `cluster::switch_cluster::feature::` namespace
                           gotcha above); not hardware-tested (though
                           this repo's other momentary-button device
                           types already confirm the underlying breadboard-
                           pushbutton wiring works on real hardware — this
                           file's own press-timing state machine on top of
                           that hasn't itself been exercised on a
                           physical board).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/refrigerator/    Refrigerator — twenty-seventh device type, and
                           this repo's first genuinely composed, multi-
                           endpoint device: a Refrigerator (0x0070) root
                           endpoint with two Temperature Controlled Cabinet
                           (0x0071) *child* endpoints (Fridge + Freezer),
                           linked via esp-matter's real parent-child
                           endpoint API — not just "two endpoints that
                           happen to exist on one node" the way firmware/
                           outlet/'s second (Electrical Sensor) endpoint is.
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/Refrigerator.xml:
                           its `<conditionRequirements>` block mandates at
                           least one child endpoint of device type
                           Temperature Controlled Cabinet with the "Cooler"
                           condition — a real, spec-level structural
                           requirement, not a design choice made here.
                           `endpoint::refrigerator::create()` confirmed to
                           add only a Descriptor cluster + the device-type-
                           list entry (Identify/RefrigeratorAndTemperature
                           ControlledCabinetMode/RefrigeratorAlarm are all
                           `<optionalConform/>`, so esp-matter's own
                           generated file's comment says outright it isn't
                           adding them by default) — Identify +
                           RefrigeratorAlarm are added manually onto the
                           root endpoint, same "add extra clusters onto an
                           already-correct endpoint" pattern established
                           repeatedly in this repo. `temperature_controlled_
                           cabinet::create()` DOES add TemperatureControl
                           automatically (mandatoryConform there) but not
                           RefrigeratorAndTemperatureControlledCabinetMode
                           or TemperatureMeasurement (both optionalConform)
                           — added manually onto each cabinet endpoint.
                           The two cabinet endpoints are linked to the root
                           via the real `esp_matter::set_parent_endpoint
                           (child, parent)` API, confirmed by reading
                           esp-matter's own official `examples/refrigerator/
                           main/app_main.cpp` reference end to end.

                           A real, previously-undocumented discrepancy
                           between esp-matter's two parallel cluster/
                           endpoint implementations was found and is worth
                           remembering for any future device type: esp-
                           matter ships a newer "generated" data model
                           (only compiled in when
                           CONFIG_ESP_MATTER_ENABLE_GENERATED_DATA_MODEL is
                           set — off by default, and left off here, same as
                           every other device type in this repo) alongside
                           an older "legacy" one that's the actual default
                           `esp_matter_cluster.h` compiles against. The
                           "generated" version's `temperature_controlled_
                           cabinet_device.cpp` sets the TemperatureNumber
                           (TN) feature flag automatically inside its own
                           `add()`, and its temperature_number config
                           field is named `temperature_setpoint` — but the
                           actual "legacy" `esp_matter_endpoint.cpp`'s own
                           `temperature_controlled_cabinet::add()` does
                           NOT set that feature flag, and its field is
                           named `temp_setpoint` instead. Both were
                           initially written assuming "generated" behavior
                           (since that's what a first read of the SDK
                           source showed) and both were real Docker-build-
                           caught compile errors, fixed by setting
                           `feature_flags` explicitly and using the legacy
                           field name. A second such discrepancy: esp-
                           matter's own C++ wrapper namespace for
                           RefrigeratorAndTemperatureControlledCabinetMode
                           is shorter in the legacy header —
                           `cluster::refrigerator_and_tcc_mode`, not
                           `::refrigerator_and_temperature_controlled_
                           cabinet_mode` — a straight "has not been
                           declared" compile error rather than a field-name
                           mismatch, fixed the same way. The underlying
                           connectedhomeip cluster server code and its
                           `ModeTag`/`Id` enum values are unaffected by
                           this split either time — only esp-matter's own
                           wrapper naming differs.

                           TemperatureControl is TN-only — the device
                           type's own XML mandates TN and explicitly
                           disallows TL (TemperatureLevel). The official
                           reference example calls `TemperatureControlCluster
                           ::SetDelegate(&sAppSupportedTemperatureLevels
                           Delegate)` regardless; this file deliberately
                           does NOT, confirmed safe by reading
                           `TemperatureControlCluster.cpp` directly rather
                           than copying the reference blindly:
                           `SetDelegate()`'s own target (`mDelegate`) is
                           only ever read in two places, both gated behind
                           the TL feature flag, which is never set here.
                           The real, always-relevant API for TN mode is
                           `SetTemperatureSetpoint()`/`GetTemperatureSetpoint()`
                           — and `HandleSetTemperature()` (the SetTemperature
                           command's own handler) already calls
                           `SetTemperatureSetpoint()` internally, so a
                           controller's command is handled entirely inside
                           the cluster with zero app code needed; this
                           file's own control loop only ever *reads* the
                           live setpoint back via `GetTemperatureSetpoint()`,
                           through the same registry-lookup-and-cast
                           pattern this repo's other code-driven-cluster
                           access already uses (confirmed: a real
                           `temperature_control/` folder exists under
                           `data_model_provider/clusters/`).
                           MinTemperature/MaxTemperature/TemperatureSetpoint
                           are all Matter's global `temperature` type
                           (int16, hundredths of a degree C, same encoding
                           firmware/thermostat/'s setpoints use). Fridge:
                           1.00-10.00 degC, default target 4.00 degC.
                           Freezer: -24.00..-14.00 degC, default target
                           -18.00 degC — ordinary commercial ranges, not
                           researched against one specific real appliance's
                           spec sheet, since there's no single "correct"
                           answer here the way a chip's register map has
                           one.

                           RefrigeratorAndTemperatureControlledCabinetMode
                           is wired the same automatic way as firmware/
                           water-heater/'s WaterHeaterMode/firmware/
                           robot-vacuum/'s RvcRunMode (`config->delegate` +
                           `InitModeDelegate()`, no ordering awareness
                           needed). Each cabinet gets its OWN instance with
                           its own 2-mode list: "Normal" (ModeTag::kAuto)
                           and either "Rapid Cool" (fridge) or "Rapid
                           Freeze" (freezer) — both tag values confirmed
                           against connectedhomeip's own generated
                           RefrigeratorAndTemperatureControlledCabinetMode/
                           Enums.h. Rather than two near-duplicate Delegate
                           subclasses, this file writes ONE parameterized
                           `RefrigeratorCabinetModeDelegate` class taking a
                           `CabinetKind` at construction — the two mode
                           lists differ only in one tag/label. "Rapid
                           Cool"/"Rapid Freeze" get no genuinely different
                           control algorithm — same "smallest reasonable
                           next step" scope cut as firmware/robot-vacuum/'s
                           "Mapping" mode: while active, the cabinet's
                           control loop simply targets 3.00 degC colder
                           than the normal setpoint, honestly simulating
                           "cool down faster" without claiming any real
                           compressor-staging logic a hobby-scale single-
                           relay design doesn't have.

                           RefrigeratorAlarm confirmed NOT code-driven (no
                           `refrigerator_alarm/` folder under
                           `data_model_provider/clusters/`) — Mask/State/
                           Supported are plain ember uint32_t bitmap
                           attributes, written via `attribute::update()`.
                           `RefrigeratorAlarm::AlarmBitmap` has exactly ONE
                           bit, `kDoorOpen` — Mask/Supported are both fixed
                           to it, State toggled live by a debounced
                           door-sensor poll (a whole-appliance reed switch,
                           not per-compartment — real combination fridge/
                           freezers commonly ship one door-open indicator
                           per exterior door, matching this scope). A real,
                           previously-undocumented gap: esp-matter's
                           `esp_matter_event_impl.h` declares a
                           `refrigerator_alarm::event::create_notify()`
                           capability but, unlike e.g. `switch_cluster`'s
                           own `send_initial_press()`, there is no matching
                           `send_notify()`-style runtime helper for firing
                           it — confirmed by reading that entire header.
                           This file does NOT hand-rig the low-level
                           connectedhomeip event API to work around that;
                           State's own attribute-change report already
                           tells a controller the alarm changed, and
                           skipping the supplementary Notify event is the
                           same "smallest reasonable next step" scope cut
                           this repo applies to other optional-richness
                           gaps (e.g. firmware/air-quality-sensor/'s
                           AirQuality FeatureMap gap).

                           TemperatureMeasurement (one instance per
                           cabinet) reuses firmware/water-heater/'s own
                           DS18B20 driver (itself from firmware/
                           thermostat/'s SENSOR_TYPE library) verbatim,
                           parameterized by GPIO instead of a single
                           hardcoded `#define` since this file genuinely
                           needs two independent sensors. Confirmed the
                           same code-driven-cluster status as firmware/
                           temperature-sensor/'s own TemperatureMeasurement
                           — `SetMeasuredValue()` via the registry-lookup-
                           and-cast pattern, parameterized by endpoint id
                           since there are two live instances on one node.

                           Hardware: two independent relay+sensor pairs
                           (REFRIGERATOR_FRIDGE_RELAY_GPIO/_FREEZER_
                           RELAY_GPIO, active-LOW, matching firmware/
                           valve/'s and firmware/water-heater/'s own relay
                           convention) — a deliberate, documented
                           simplification, since a real fridge/freezer
                           combination almost always shares ONE compressor
                           and refrigerant loop with a damper, not two
                           fully independent cooling circuits; modelling
                           real shared-compressor thermodynamics is out of
                           scope for a hobby retrofit, same reasoning
                           firmware/thermostat/'s RELAY output already
                           applies to a boiler's own internals. One
                           reusable control-loop task body
                           (`cabinet_control_task`) is spawned twice with a
                           different `cabinet_runtime_t` argument each
                           time, rather than writing two near-duplicate
                           tasks. 0.5 degC hysteresis reuses firmware/
                           water-heater/'s own reasoning (slow thermal
                           mass, avoid relay chatter); a sensor read
                           failure turns that cabinet's own cooling off
                           (fail-safe), same convention firmware/
                           water-heater/'s control loop already
                           established. Standard quick-power-cycle factory
                           reset. Build-verified in Docker (two real,
                           sequential sets of compile errors caught and
                           fixed — see the "legacy vs. generated" gap
                           above); not hardware-tested (no relay/DS18B20/
                           reed-switch hardware for this device type
                           physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/dishwasher/      Dishwasher — twenty-eighth device type, and this
                           repo's first over the *generic* OperationalState
                           cluster (0x0060 — the same base cluster
                           RvcOperationalState derives from, but used here
                           directly, un-derived) plus a genuinely new
                           command-cycle shape: Start/Stop/Pause/Resume
                           driving a real, if simplified, wash cycle,
                           rather than a continuous regulation loop
                           (thermostat/water-heater/refrigerator) or a
                           one-shot actuation (valve/door-lock).
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/Dishwasher.xml: only
                           OperationalState is `<mandatoryConform/>` (with
                           its own OperationCompletion event also
                           mandatory, per this device type's own revision
                           2) — Identify, On/Off (DeadFrontOnOff feature
                           only — a narrow "is the device's own UI/front-
                           panel powered" semantic, not a real power
                           switch, left out), TemperatureControl,
                           DishwasherMode, and DishwasherAlarm are all
                           `<optionalConform/>`. A real naming gotcha found
                           while researching this: esp-matter's own
                           generated files use `dish_washer` (with an
                           underscore) throughout — the endpoint helper is
                           `esp_matter::endpoint::dish_washer::create()`,
                           the mode cluster is `cluster::dish_washer_mode`,
                           the alarm cluster is
                           `cluster::dish_washer_alarm` — none matching the
                           CSA's own un-underscored "Dishwasher"/
                           "DishwasherMode"/"DishwasherAlarm" naming; a
                           first source search for "dishwasher" (no
                           underscore) came back nearly empty as a result.
                           The underlying connectedhomeip cluster server
                           code and its own C++ namespaces use the CSA's
                           spelling as expected — only esp-matter's own
                           wrapper file/namespace names differ.
                           `endpoint::dish_washer::create()` confirmed to
                           add only a Descriptor cluster + OperationalState
                           (with OperationCompletion pre-registered) via
                           `common::create<T>()` — Identify +
                           TemperatureControl + DishwasherMode +
                           DishwasherAlarm are all added manually onto that
                           same endpoint afterward.

                           OperationalState itself surfaced a SIXTH
                           genuinely distinct "how do I reach a live
                           cluster instance from app code" pattern in this
                           repo: unlike firmware/robot-vacuum/'s
                           RvcOperationalState (whose esp-matter `config_t`
                           is a literally empty struct, needing a hand-
                           built raw `Instance`+`Delegate` pair), the
                           generic OperationalState cluster DOES support
                           `config->delegate` (confirmed by reading
                           `esp_matter_cluster.cpp`'s own
                           `operational_state::create()` directly — the
                           same `set_delegate_and_init_callback()` pattern
                           every other auto-wired delegate cluster in this
                           repo already uses). Ported directly from
                           connectedhomeip's own real reference
                           (`examples/dishwasher-app/dishwasher-common/
                           src/operational-state-delegate-impl.cpp`, read
                           end to end): each `HandleXStateCallback` calls
                           `GetInstance()->SetOperationalState(...)`
                           directly and sets `err` accordingly — the
                           Delegate itself owns the state transition,
                           unlike e.g. firmware/door-lock/'s LockState or
                           firmware/valve/'s CurrentState (both reported
                           back through a separate call after the
                           framework's own command handling already ran).
                           A registry-style lookup is still needed for the
                           two places this file touches the cluster from
                           OUTSIDE the delegate's own callbacks — the door
                           sensor's own async safety-pause, and the wash-
                           cycle finishing on its own — via esp-matter's
                           own public `get_delegate_managed_instance
                           (cluster::get(endpoint_id, OperationalState::Id))`
                           (declared in `esp_matter_data_model.h`), the
                           sixth pattern: (1) plain registry-lookup setter,
                           (2) a Delegate whose own reporting call happens
                           to be a working generic free-function proxy,
                           (3) the `chip::app::…registry().Get()`-based
                           fallback for `DefaultServerCluster`-derived
                           clusters, (4) a cluster-family-specific
                           convenience free function
                           (`ResourceMonitoring::GetClusterInstance()`),
                           (5) a direct FeatureMap `attribute::update()`
                           override, and now (6)
                           `get_delegate_managed_instance()` for a legacy-
                           ember-style cluster whose live C++ instance is
                           delegate-managed but not one of
                           connectedhomeip's `DefaultServerCluster`-
                           registry clusters at all (confirmed: no
                           `operational_state/` folder exists under
                           `data_model_provider/clusters/`). Four real
                           compile errors were caught and fixed across two
                           failed Docker build attempts, not guessed: a
                           missing `#include <app/clusters/
                           operational-state-server/CodegenIntegration.h>`
                           (the header declaring `Delegate`/`Instance`/
                           `GenericOperationalState`/`GenericOperationalError`
                           at all — without it, the compiler silently
                           resolved `err`'s type to plain `int&` instead of
                           erroring outright, a genuinely confusing failure
                           mode worth remembering); `DishwasherAlarmServer`
                           needing its full `DishwasherAlarm::
                           DishwasherAlarmServer` qualification (nested
                           inside the `DishwasherAlarm` namespace, not
                           reachable through a top-level `using namespace
                           chip::app::Clusters;` alone); esp-matter's own
                           `get_delegate_managed_instance()` needing its
                           full `esp_matter::cluster::` qualification too;
                           and a missing `GetCountdownTime()` pure-virtual
                           override (returns null — no live countdown
                           reported, a documented scope cut).

                           TemperatureControl (TN-only, wash-temperature
                           target: 30.00-70.00 degC, default 50.00 degC)
                           reuses firmware/refrigerator/'s exact pattern,
                           including its own documented legacy-vs-generated
                           `feature_flags`/`temp_setpoint` pitfall — set
                           explicitly here for the same reason.
                           DishwasherMode is a fifth ModeBase-derived
                           cluster in this repo (same automatic
                           `config->delegate` wiring as every prior one),
                           offering three real dishwasher-specific tags
                           confirmed directly against connectedhomeip's own
                           generated `DishwasherMode/Enums.h` — "Normal"
                           (`kNormal`), "Heavy" (`kHeavy`), "Light"
                           (`kLight`) — plus a real business rule borrowed
                           from firmware/robot-vacuum/'s own "reject a
                           clean-mode change while actively cleaning" rule:
                           `HandleChangeToMode()` rejects with
                           `ModeBase::StatusCode::kInvalidInMode` whenever
                           OperationalState is Running.

                           DishwasherAlarm turned out to have a genuinely
                           complete Delegate + Server API — unlike
                           firmware/refrigerator/'s own RefrigeratorAlarm,
                           confirmed by reading `dishwasher-alarm-server.h`
                           directly: `DishwasherAlarmServer::Instance()
                           ::SetStateValue()` both sets the State attribute
                           AND fires the cluster's own Notify event
                           internally — no manual event-rigging gap this
                           time. All six real `AlarmBitmap` bits (InflowError/
                           DrainError/DoorError/TempTooLow/TempTooHigh/
                           WaterLevelError, confirmed against
                           connectedhomeip's own generated Enums.h) are
                           declared Supported/Mask at startup, but only
                           DoorError is ever actually asserted (driven by a
                           real reed-switch door sensor) — the other five
                           would each need real flow/level/temperature-
                           fault sensing hardware this hobby-scale build
                           doesn't have, same "smallest reasonable next
                           step" scope cut as firmware/evse/'s own always-
                           `NoError` `FaultState`. `dish_washer_alarm
                           ::create()`'s own FeatureMap is hardcoded to 0
                           (same documented gap class as RefrigeratorAlarm/
                           AirQuality) — the Reset feature is therefore
                           never advertised, so DoorError only ever clears
                           via the door physically closing again, not via a
                           controller-issued ResetAlarms command.

                           The wash cycle itself is a real, if deliberately
                           simplified, timed state machine: Washing (heater
                           relay, hysteresis-controlled against a DS18B20
                           reading and TemperatureControl's own live
                           setpoint, same 0.5 degC hysteresis convention
                           firmware/refrigerator/'s and firmware/
                           water-heater/'s own control loops already use,
                           plus the wash pump running continuously) for 45
                           minutes, then Draining (drain pump only) for 5
                           minutes, then `OnOperationCompletionDetected()`
                           back to Stopped — no Fill phase or turbidity/
                           soil-sensing logic is modelled at all (a real
                           water-inlet valve + float switch would be needed
                           for a genuine Fill phase), same honest,
                           documented scope cut as firmware/robot-vacuum/'s
                           own "no real navigation" limitation.
                           `HandleStartStateCallback()` rejects the command
                           if the door is open, matching real dishwasher
                           interlock behavior; opening the door mid-cycle
                           doesn't error the OperationalState at all — it
                           PAUSES it (a real, physically-correct dishwasher
                           behavior, not an error condition) while
                           separately raising DishwasherAlarm's DoorError
                           bit, with no silent auto-resume once the door
                           closes again (the user must explicitly resume).

                           Standard quick-power-cycle factory reset.
                           Build-verified in Docker (four real compile
                           errors caught and fixed across two failed
                           attempts — see above); not hardware-tested (no
                           relay/DS18B20/reed-switch hardware for this
                           device type physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/laundry-washer/  Laundry Washer — twenty-ninth device type, and the
                           closest sibling to firmware/dishwasher/ in this
                           repo: the same generic OperationalState cluster
                           (0x0060), the same TemperatureControl (TN-only)
                           pattern, and a fourth ModeBase-derived Mode
                           cluster — plus one genuinely new cluster,
                           LaundryWasherControls, whose SpinSpeed/
                           NumberOfRinses settings this file actually gives
                           real physical meaning to.
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/LaundryWasher.xml:
                           only OperationalState is `<mandatoryConform/>`
                           (with OperationCompletion also mandatory, per
                           this device type's own revision 2) — Identify,
                           On/Off (DeadFrontOnOff feature only, skipped for
                           the same reason firmware/dishwasher/'s own
                           header comment documents), LaundryWasherMode,
                           LaundryWasherControls, and TemperatureControl
                           are all `<optionalConform/>`. Confirmed by
                           reading `esp_matter_endpoint.cpp`'s own
                           `laundry_washer::add()` directly: structurally
                           identical to `dish_washer::add()` (both share
                           `config_t = app_with_operational_state_config`
                           and only create OperationalState) — Identify +
                           TemperatureControl + LaundryWasherMode +
                           LaundryWasherControls are all added manually
                           afterward, same pattern. Unlike firmware/
                           dishwasher/'s own `dish_washer`/`dish_washer_
                           mode`/`dish_washer_alarm` naming gotcha,
                           esp-matter's own wrapper names here
                           (`laundry_washer`, `laundry_washer_mode`,
                           `laundry_washer_controls`) DO match the CSA's
                           own naming (standard snake_case) — confirmed by
                           reading the legacy header directly before
                           assuming either way held generally.

                           OperationalState itself is an identical pattern
                           to firmware/dishwasher/'s own — same automatic
                           `config->delegate` wiring, same
                           `get_delegate_managed_instance()` lookup for the
                           two places this file touches the cluster from
                           outside the delegate's own callbacks (the door
                           sensor's async safety-pause, and the wash cycle
                           finishing on its own — the sixth "reach a live
                           cluster instance from app code" pattern
                           firmware/dishwasher/'s own repository-layout
                           entry catalogues in full).
                           `HandleStartStateCallback()`/
                           `HandleResumeStateCallback()` reject with
                           `ErrorStateEnum::kUnableToStartOrResume` if the
                           door is open; opening the door mid-cycle PAUSES
                           rather than errors the OperationalState (real
                           front-loader behavior) — but unlike Dishwasher,
                           LaundryWasher's own device type XML lists no
                           Alarm cluster at all, so there's no DoorError-
                           style bit to raise here; the Pause itself is the
                           only signal a controller gets.

                           TemperatureControl (TN-only, wash-temperature
                           target: 20.00-60.00 degC, default 40.00 degC)
                           reuses firmware/dishwasher/'s and firmware/
                           refrigerator/'s exact pattern, including the
                           same documented legacy-vs-generated
                           `feature_flags`/`temp_setpoint` handling.
                           LaundryWasherMode is a fourth ModeBase-derived
                           cluster in this repo (same automatic wiring as
                           every prior one), offering four real mode tags
                           confirmed directly against connectedhomeip's own
                           generated `LaundryWasherMode/Enums.h` — "Normal"
                           (`kNormal`), "Delicate" (`kDelicate`), "Heavy"
                           (`kHeavy`), "Whites" (`kWhites`) — a genuinely
                           different, larger tag set than Dishwasher's own
                           three. Same `kInvalidInMode`-while-Running
                           rejection rule as firmware/dishwasher/'s own
                           DishwasherMode.

                           LaundryWasherControls is a genuinely new cluster
                           for this repo, and the one setting here that
                           actually changes physical behavior. Confirmed by
                           reading `esp_matter_cluster.cpp`'s own
                           `laundry_washer_controls::create()` directly:
                           unlike RefrigeratorAlarm/AirQuality's own
                           documented FeatureMap-hardcoded-to-0 gap, this
                           cluster's FeatureMap is set TWICE — once
                           hardcoded to 0, then immediately overwritten
                           with `config->feature_flags` — so there's no
                           real gap here, `feature_flags` just needs to be
                           set explicitly (both Spin and Rinse features are
                           enabled; the cluster's own
                           `VALIDATE_FEATURES_AT_LEAST_ONE("Spin,Rinse",
                           ...)` check confirms at least one is required).
                           `SpinSpeedCurrent`/`NumberOfRinses` are plain
                           ember attributes (confirmed: the cluster
                           registers a `MatterLaundryWasherControlsCluster
                           ServerPreAttributeChangedCallback`, the same
                           PRE_ATTRIBUTE_CHANGED-hook shape OnOff/
                           LevelControl already use, not a code-driven
                           `DefaultServerCluster`) — a controller can write
                           either directly, no command needed. This cluster
                           ALSO has a real Delegate
                           (`GetSpinSpeedAtIndex()`/
                           `GetSupportedRinseAtIndex()`, confirmed by
                           reading `laundry-washer-controls-delegate.h`
                           directly) supplying the *supported-value lists*
                           (`SpinSpeeds`/`SupportedRinses`) — the same
                           "feature-flag-gated attributes plus a separate
                           Delegate for the supported-list" shape
                           TemperatureControl's own TL feature uses,
                           confirmed as a real, distinct pattern rather
                           than assumed identical to any single-mechanism
                           cluster elsewhere in this repo. `SpinSpeeds`
                           offers four real options ("Off"/"Low"/"Medium"/
                           "High") — purely informational (no real
                           variable-speed motor control, same "smallest
                           reasonable next step" scope cut as firmware/
                           robot-vacuum/'s fixed-speed drive motors).
                           `SupportedRinses` offers all four real
                           `NumberOfRinsesEnum` values (`kNone`/`kNormal`/
                           `kExtra`/`kMax`, confirmed against
                           connectedhomeip's own generated Enums.h) — and
                           UNLIKE SpinSpeedCurrent, `NumberOfRinses`
                           genuinely drives this file's own wash-cycle
                           simulation: tracked via the same
                           `attribute::PRE_UPDATE` pattern firmware/
                           water-heater/'s own SystemMode tracking already
                           established, it sets how many real Rinse-then-
                           Drain phases the cycle actually runs (0/1/2/3
                           for None/Normal/Extra/Max respectively).

                           The wash cycle itself: Washing (heater relay,
                           hysteresis-controlled against a DS18B20 reading
                           and TemperatureControl's own live setpoint, same
                           0.5 degC hysteresis convention used throughout
                           this repo's control loops, plus the motor
                           running), then however many rinses
                           `NumberOfRinses` currently calls for (each a
                           real Rinsing-then-Draining phase pair), then
                           Spinning (motor only, no heater), before calling
                           `OnOperationCompletionDetected()` and returning
                           to Stopped. A single motor relay drives both
                           agitation (Washing/Rinsing) and spin (Spinning)
                           — a deliberate, documented simplification: real
                           washing machines use a variable-speed/reversible
                           motor and a clutch/gearbox to switch between the
                           two, which this hobby-scale single-relay build
                           doesn't model, the same "smallest reasonable
                           next step" reasoning firmware/refrigerator/'s
                           own single-relay-per-compartment design already
                           applies. No Filling phase is modelled at all
                           (a real water-inlet valve + level sensor would
                           be needed), same honest scope cut as firmware/
                           dishwasher/'s own skipped Fill phase.

                           Standard quick-power-cycle factory reset.
                           Build-verified in Docker — clean on the first
                           attempt, the careful upfront research into
                           firmware/dishwasher/'s own hard-won lessons
                           (legacy-vs-generated pitfalls, the
                           `CodegenIntegration.h` include, the
                           `get_delegate_managed_instance()` qualification)
                           paying off directly; not hardware-tested (no
                           relay/DS18B20/reed-switch hardware for this
                           device type physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/pump/             Pump — thirtieth device type, and this repo's
                           first over the PumpConfigurationAndControl
                           cluster — also its first plain continuous-
                           control-loop-free device type in three
                           sessions (after Dishwasher/Laundry Washer/
                           Refrigerator's own OperationalState- or
                           hysteresis-loop-driven designs): a pump reacts
                           directly to a controller's own On/Off,
                           CurrentLevel, and OperationMode writes, with no
                           background task of its own at all.
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/Pump.xml:
                           Identify, On/Off, and
                           PumpConfigurationAndControl are all
                           `<mandatoryConform/>` — a real, non-optional
                           trio, unlike almost every other device type
                           this repo has built, where Identify alone is
                           consistently optional. `endpoint::pump::
                           create()` confirmed complete/ready-to-use by
                           reading esp_matter_endpoint.cpp's own
                           `pump::add()` directly (Identify + OnOff +
                           PumpConfigurationAndControl, auto-Descriptor
                           via `common::create<T>()`) — and, a genuinely
                           new detail for this repo, `pump::config_t`'s
                           own constructor already sets
                           `identify.identify_type = Identify::
                           IdentifyTypeEnum::kActuator` itself, the first
                           top-level helper here whose own constructor
                           sets Identify's type rather than the call site
                           doing it explicitly. LevelControl (pump speed)
                           is added manually onto the same endpoint
                           afterward — same "add extra clusters onto an
                           already-correct endpoint" pattern used
                           throughout this repo — using the plain legacy
                           `level_control::config_t` with no Lighting
                           feature at all (confirmed by reading that
                           config_t directly: a pump genuinely doesn't
                           need it, unlike firmware/dimmable-light/'s own
                           Lighting-feature setup). PumpConfigurationAndControl
                           enables only the ConstantSpeed (SPD) feature —
                           the one of the cluster's five "choice, at
                           least 1" control-mode features
                           (ConstantPressure/CompensatedPressure/
                           ConstantFlow/ConstantSpeed/ConstantTemperature)
                           that maps directly onto a PWM-driven speed
                           with no pressure/flow/temperature sensor
                           hardware needed — same "smallest reasonable
                           next step" scoping as every other device
                           type's own first-cut feature choice.
                           FeatureMap is confirmed set correctly here
                           (`create()` threads `config->feature_flags`
                           through directly) — unlike RefrigeratorAlarm/
                           AirQuality's own documented hardcoded-to-0
                           gap. MaxSpeed/MinConstSpeed/MaxConstSpeed use
                           a plain 0-100 percent scale (the attribute
                           carries no unit requirement, confirmed against
                           its own uint16 definition) rather than a
                           hardware-specific RPM figure, since no
                           specific real pump motor's rated speed was
                           being modelled. The cluster's own seventeen-
                           event fault-reporting set (SupplyVoltageLow/
                           High, DryRunning, PumpBlocked,
                           MotorTemperatureHigh, Leakage, AirDetection,
                           etc.) is not fired anywhere — every one needs
                           real fault-detection hardware (current
                           sensing, pressure transducers, thermal
                           cutouts) this hobby-scale build doesn't have,
                           the same "no sensor, no fabricated fault
                           reporting" honesty precedent firmware/evse/'s
                           always-NoError FaultState and firmware/
                           smoke-co-alarm/'s simple heuristic already
                           establish. OperationMode (Normal/Minimum/
                           Maximum/Local) is a plain writable ember
                           attribute that genuinely drives the output:
                           Normal uses LevelControl's own CurrentLevel as
                           the speed target (a controller's speed slider
                           maps straight onto PWM duty); Minimum/Maximum
                           instead use the feature-configured
                           MinConstSpeed/MaxConstSpeed bounds, ignoring
                           CurrentLevel entirely, matching the spec's own
                           wording for those two modes; Local is accepted
                           but behaves identically to Normal, logged as
                           such (no separate physical control panel to
                           defer to) — an honest, documented scope cut
                           rather than silently ignoring the write.
                           EffectiveOperationMode mirrors OperationMode
                           1:1 (correct here since neither Automatic nor
                           LocalOperation is enabled); EffectiveControlMode
                           is fixed to ConstantSpeed at startup and never
                           changes. `PUMP_RELAY_GPIO` (active-LOW, this
                           repo's established relay convention) gates the
                           pump motor's own power entirely, separate from
                           `PUMP_SPEED_PWM_GPIO`'s real PWM speed signal
                           (via ESP-IDF's driver/ledc.h, the same LEDC
                           peripheral firmware/dimmable-light/'s and
                           firmware/fan/'s own outputs already use) —
                           matching how a real variable-speed pump/
                           circulator commonly exposes both a plain
                           enable line and a separate 0-10V/PWM speed
                           input. `apply_pump_output()` is the single,
                           direct call site every relevant PRE_UPDATE
                           handler funnels into, recomputing the real
                           duty cycle immediately on any OnOff/
                           CurrentLevel/OperationMode change — no
                           periodic task needed at all, unlike every
                           hysteresis-loop device type this repo has
                           built recently, since a pump's speed output is
                           a direct, immediate function of those three
                           attributes rather than something needing
                           re-evaluation against a live sensor reading on
                           a timer. Standard quick-power-cycle factory
                           reset. Build-verified in Docker; not
                           hardware-tested (no pump/relay/PWM-speed-
                           controller hardware for this device type
                           physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/laundry-dryer/    Laundry Dryer — thirty-first device type, and the
                           closest sibling to firmware/laundry-washer/ in
                           this repo: the same generic OperationalState
                           cluster (0x0060), the same TemperatureControl
                           (TN-only) pattern — but the CSA's own device
                           type XML reuses LaundryWasher's own Mode cluster
                           verbatim rather than defining a dryer-specific
                           one, and this is this repo's first laundry
                           appliance with no water handling at all (no
                           fill, no rinse, no drain — just heat and
                           tumble).
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/LaundryDryer.xml:
                           only OperationalState is `<mandatoryConform/>`
                           (OperationCompletion event also mandatory, per
                           this device type's own revision 2) — Identify,
                           On/Off (DeadFrontOnOff feature only, same skip
                           as firmware/dishwasher/'s and firmware/
                           laundry-washer/'s own identical reasoning),
                           Laundry Dryer Controls, Laundry Washer Mode, and
                           TemperatureControl are all `<optionalConform/>`.
                           `endpoint::laundry_dryer::add()` confirmed
                           structurally identical to `dish_washer::add()`/
                           `laundry_washer::add()` (all three share
                           `config_t = app_with_operational_state_config`
                           and only create OperationalState) — Identify +
                           TemperatureControl + LaundryWasherMode +
                           LaundryDryerControls all added manually
                           afterward, same pattern. A real, spec-level
                           detail worth remembering: the device type XML's
                           own cluster entry is literally `<cluster
                           id="0x0051" name="Laundry Washer Mode" .../>` —
                           NOT a separate "Laundry Dryer Mode" cluster,
                           confirmed by reading the XML directly rather
                           than assumed from the device type's own name.
                           Two constraints layered onto that reused
                           cluster for this device type (DEPONOFF feature
                           and StartUpMode attribute, both
                           `<disallowConform/>`) needed no code to enforce
                           — `laundry_washer_mode::create()` hardcodes
                           FeatureMap to 0 (no `feature_flags` field at all
                           — DEPONOFF was never reachable through this
                           helper) and never creates a StartUpMode
                           attribute either, so both are already satisfied
                           by the helper's own narrower scope, confirmed
                           by reading `esp_matter_cluster.cpp` directly.
                           Mode tags offered: Normal/Delicate/Heavy (reused
                           directly) plus Quick (`ModeTag::kQuick`, one of
                           `ModeBase`'s own *common* tags) in place of
                           Whites — Whites is a wash-specific concept
                           (bleach-safe water temperature) that doesn't
                           translate to a drying setting, confirmed by
                           reading the full `LaundryWasherMode::ModeTag`
                           enum directly before picking a fourth option.
                           Same `kInvalidInMode`-while-Running rejection
                           rule as firmware/dishwasher/'s and firmware/
                           laundry-washer/'s own Mode delegates; purely
                           informational here, same scope cut as
                           firmware/laundry-washer/'s own SpinSpeedCurrent.
                           OperationalState itself is the identical pattern
                           to firmware/dishwasher/'s and firmware/
                           laundry-washer/'s own (same automatic
                           `config->delegate` wiring, same
                           `get_delegate_managed_instance()` lookup, same
                           door-sensor safety-pause behavior — no Alarm
                           cluster on this device type either, so the
                           Pause itself is the only signal a controller
                           gets) — but drives a 2-phase Drying+Cooldown
                           cycle instead of the washer's multi-rinse one.
                           TemperatureControl (TN-only, drying-air target:
                           40.00-80.00 degC, default 60.00 degC — ordinary
                           real tumble-dryer heater-air range) reuses the
                           exact legacy-vs-generated `feature_flags`/
                           `temp_setpoint` handling documented in full
                           elsewhere in this repo. LaundryDryerControls'
                           SelectedDrynessLevel is the one attribute this
                           file gives real physical meaning to (Low/
                           Normal/Extra/Max map to 20/35/50/65 minutes of
                           drying, tracked via `attribute::PRE_UPDATE`,
                           same "one setting genuinely drives the cycle"
                           precedent firmware/laundry-washer/'s own
                           NumberOfRinses established) — SupportedDryness
                           Levels is a real Delegate-served list
                           (`GetSupportedDrynessLevelAtIndex()`, offering
                           all four real values). A real, previously-
                           undocumented gotcha was found by reading
                           `laundry-dryer-controls-server.cpp` directly
                           rather than assuming it behaves like
                           LaundryWasherControls: its own
                           `PreAttributeChangedCallback` calls
                           `VerifyOrDie(delegate != nullptr)` before
                           validating a SelectedDrynessLevel write against
                           the supported list — a controller writing this
                           attribute with NO delegate registered would
                           abort the whole device, not just silently
                           no-op, unlike every other optional-delegate
                           cluster in this repo; `config_t.delegate` is
                           therefore always set here. FeatureMap is
                           hardcoded to 0 (this cluster defines no
                           features at all, confirmed against its own
                           cluster XML) — nothing to enable, unlike
                           LaundryWasherControls' Spin/Rinse pair.
                           `laundry_dryer_task()` drives two real relay
                           outputs (heater + drum motor, both active-LOW)
                           through Drying (heater, hysteresis-controlled
                           against a DS18B20 reading and
                           TemperatureControl's own live setpoint, same
                           0.5 degC hysteresis convention used throughout
                           this repo, plus the motor tumbling) for however
                           long SelectedDrynessLevel calls for, then a
                           fixed unheated Cooldown (motor only) — a real,
                           standard tumble-dryer behavior (reduces
                           wrinkling and hot-lint fire risk), not invented
                           for this file — before
                           `OnOperationCompletionDetected()` and returning
                           to Stopped. Only two relays needed (no drain
                           pump, no Fill/Rinse phase) — this repo's first
                           laundry appliance with no water handling at
                           all. Standard quick-power-cycle factory reset.
                           Build-verified in Docker — clean on the first
                           attempt; not hardware-tested (no relay/DS18B20/
                           reed-switch hardware for this device type
                           physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/room-air-conditioner/  Room Air Conditioner — thirty-second device
                           type, and this repo's first to combine
                           firmware/thermostat/'s own Thermostat control-
                           loop pattern with firmware/fan/'s own FanControl
                           Delegate pattern on a single endpoint.
                           Recommended twice before (during firmware/evse/'s
                           and firmware/water-heater/'s own AskUserQuestion
                           rounds) but not chosen until now.
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/
                           RoomAirConditioner.xml: Identify, On/Off
                           (DeadFrontOnOff feature), and Thermostat are all
                           `<mandatoryConform/>` — Groups, Scenes
                           Management, FanControl, Thermostat User
                           Interface Configuration, HEPA/Activated Carbon
                           Filter Monitoring, Temperature Measurement, and
                           Relative Humidity Measurement are all
                           `<optionalConform/>`.
                           `endpoint::room_air_conditioner::create()`
                           confirmed complete/ready-to-use by reading
                           `esp_matter_endpoint.cpp`'s own
                           `room_air_conditioner::add()` directly: Identify
                           + OnOff (DeadFrontOnOff feature added, On +
                           Toggle commands explicitly created — Off is
                           already part of the base OnOff cluster, same as
                           firmware/pump/'s own On/Off) + Thermostat (with
                           the Cooling feature force-added via
                           `config->thermostat.feature_flags |=
                           feature::cooling::get_id()` inside `add()`
                           itself), auto-Descriptor via `common::
                           create<T>()`. A real, previously-unseen spec
                           detail in this repo: unlike every appliance
                           device type here (Dishwasher/Laundry Washer/
                           Laundry Dryer), where On/Off's DeadFrontOnOff
                           feature is optionalConform and simply left out,
                           this device type makes BOTH that On/Off cluster
                           AND a separate Thermostat SystemMode
                           mandatoryConform at once — two genuinely
                           different concepts of "on" coexist on the same
                           endpoint. No GPIO is tied to the OnOff/
                           DeadFrontOnOff attribute at all (left exactly as
                           the top-level helper wires it — a cosmetic
                           "is the unit's own display lit" flag with no
                           physical effect in this hobby build); the actual
                           cooling output is driven entirely by
                           Thermostat's own SystemMode further down.
                           ControlSequenceOfOperation is CoolingOnly (0x00)
                           — SystemMode is therefore only ever meaningfully
                           Off or Cool, any other value treated as Off
                           rather than guessed at, same scope cut
                           firmware/thermostat/'s own SystemMode handling
                           already uses. LocalTemperature/SystemMode/
                           OccupiedCoolingSetpoint are all plain ember
                           attributes (no `thermostat/` folder under
                           `data_model_provider/clusters/`, the same check
                           firmware/thermostat/'s own header comment
                           documents in full) — same `attribute::
                           PRE_UPDATE` + `attribute::update()` pattern used
                           throughout this repo. `ROOM_AC_SENSOR_GPIO`
                           reuses the exact DS18B20 1-Wire driver firmware/
                           laundry-dryer/'s (itself firmware/thermostat/'s/
                           firmware/water-heater/'s) own DS18B20 path
                           already establishes verbatim — deliberately just
                           this one sensor rather than firmware/
                           thermostat/'s full 7-chip `SENSOR_TYPE` library,
                           same "smallest reasonable next step" scope cut
                           firmware/water-heater/'s own tank-probe choice
                           already applies. `ROOM_AC_HYSTERESIS_
                           CENTIDEGREES` (0.3 degC) matches firmware/
                           thermostat/'s own default.

                           FanControl is `<optionalConform/>` here (unlike
                           firmware/fan/'s/firmware/air-purifier/'s/
                           firmware/extractor-hood/'s own device types,
                           where it's mandatory and auto-wired by their own
                           top-level helpers) — `room_air_conditioner::
                           config_t` has no `fan_control` field at all,
                           confirmed by reading the header directly, so
                           this file adds it manually via the lower-level
                           `cluster::fan_control::create()` free function
                           instead, the usual "add extra clusters onto an
                           already-correct endpoint" pattern. That cluster-
                           level `config_t` DOES expose its own `delegate`
                           field (wired via `set_delegate_and_init_
                           callback()` at create time) — but this file
                           deliberately does NOT rely on that, following
                           firmware/air-purifier/'s and firmware/
                           extractor-hood/'s own proven-correct convention
                           instead: `config.delegate` stays null at create
                           time, and the real Delegate is attached
                           afterward via connectedhomeip's own
                           `FanControl::SetDefaultDelegate()` free
                           function, called only AFTER `esp_matter::
                           start()` — see firmware/fan/'s own header
                           comment for the full, hard-won story of why
                           calling it any earlier is a silent no-op.
                           `HandleStep()`/`OnFanDriveStateChanged()`/the
                           PercentSetting-only scope/the `OffLowMedHigh`
                           FanModeSequence choice are all reused verbatim
                           from firmware/fan/. The fan's own PercentSetting
                           is deliberately NOT coupled to Thermostat's own
                           cool demand — a controller can run the fan alone
                           (real room ACs commonly support this).

                           `ROOM_AC_COMPRESSOR_RELAY_GPIO` (active-LOW) is
                           gated purely by the Cool-only hysteresis loop —
                           a real compressor contactor/relay module, not
                           the mandatory OnOff/DeadFrontOnOff attribute.
                           `ROOM_AC_FAN_PWM_GPIO` is real PWM via ESP-IDF's
                           driver/ledc.h, the same LEDC peripheral
                           firmware/fan/'s own output already uses.
                           Standard quick-power-cycle factory reset.
                           Build-verified in Docker — clean on the first
                           attempt; not hardware-tested (no relay/DS18B20/
                           fan-driver hardware for this device type
                           physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/heat-pump/       Heat Pump — thirty-third device type, and this
                           repo's second genuinely composed, multi-endpoint
                           device after firmware/refrigerator/ — but
                           composed very differently, and against real,
                           conflicting guidance from three separate sources
                           that had to be weighed against each other before
                           writing any code.
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/HeatPump.xml: the
                           ROOT endpoint itself is nearly empty — just
                           Identify (optionalConform) and a CLIENT-side
                           Thermostat binding (optionalConform). All the
                           real substance is in two `composedDeviceTypes`
                           entries: an Electrical Sensor (0x0510,
                           ElectricalPowerMeasurement +
                           ElectricalEnergyMeasurement both
                           mandatoryConform) and a Thermostat (0x0301, with
                           an EXTRA mandatoryConform User Label cluster
                           layered on top of that device type's own normal
                           requirements). esp-matter's own `endpoint::
                           heat_pump::create()` (confirmed by reading
                           `esp_matter_endpoint.cpp`'s own `heat_pump::
                           add()` directly) does NOT follow that structure
                           literally: it composes PowerSource +
                           ElectricalSensor (EPM+EEM) + DeviceEnergyManagement
                           [PowerAdjustment] all onto the SAME root
                           endpoint — confirmed by reading `electrical_
                           sensor::add()` itself, which calls
                           `add_device_type()` on whatever endpoint it's
                           handed rather than creating a child — a
                           genuinely different composition style from
                           firmware/refrigerator/'s own child-endpoint
                           pattern for Temperature Controlled Cabinet. It
                           implements NO Thermostat composition at all.
                           Cross-checked against connectedhomeip's own chef
                           reference device (`rootnode_heatpump_
                           87ivjRAECh.matter`, fetched and read directly),
                           which CONFIRMS esp-matter's own same-endpoint
                           composition choice for PowerSource/
                           ElectricalSensor/DeviceEnergyManagement — but for
                           temperature sensing uses two entirely separate
                           `ma_tempsensor` (Temperature Sensor, 0x0302)
                           child endpoints instead of a composed Thermostat
                           device type at all, an older/alternate
                           interpretation that doesn't match the current
                           ratified XML's own explicit composed-Thermostat-
                           with-UserLabel requirement. With three real
                           sources disagreeing, this file follows the
                           CURRENT ratified 1.6 XML's own stated intent (a
                           composed Thermostat child) rather than chef's
                           older reference — implemented as a genuine CHILD
                           endpoint via `esp_matter::set_parent_endpoint
                           (child, parent)`, the same API firmware/
                           refrigerator/'s own Fridge/Freezer children
                           already establish — while keeping esp-matter's
                           own proven, tested same-endpoint composition for
                           the Electrical Sensor part (confirmed correct by
                           both esp-matter's own implementation AND the
                           independent chef reference agreeing on that
                           specific point). A UserLabel cluster is added
                           manually onto the Thermostat child endpoint
                           (confirmed `cluster::user_label::create()` uses
                           a trivial empty `common::config_t`, no special
                           setup needed).

                           Root endpoint: `endpoint::heat_pump::create()`
                           handles PowerSource (wired feature),
                           ElectricalSensor (both EPM+EEM enabled via
                           `config->electrical_sensor.optional_clusters_mask`
                           — mandatory per the XML; the helper itself sets
                           ElectricalPowerMeasurement's own
                           AlternatingCurrent feature bit internally), and
                           DeviceEnergyManagement[PowerAdjustment] — all
                           with zero extra app code needed. Deliberately
                           NOT driven by any real sensor: both clusters'
                           legacy `cluster::create()` functions tolerate a
                           null `delegate` (confirmed by reading
                           `esp_matter_cluster.cpp` directly — no
                           `VerifyOrDie`), so both exist and report their
                           static/zero default values with no crash risk —
                           same "no sensor, no fabricated data" honesty
                           precedent firmware/evse/'s own always-NoError
                           FaultState already establishes; a real product
                           wanting genuine power telemetry here would want
                           firmware/outlet/'s own hand-written
                           ElectricalPowerMeasurement::Instance/Delegate
                           pair (or one of its 6 real power-monitor chip
                           drivers) instead. Identify is added manually
                           onto the root (the top-level helper doesn't
                           auto-add it).

                           Thermostat child endpoint: Heat+Cool (unlike
                           firmware/room-air-conditioner/'s own deliberately
                           Cool-only scope — a heat pump's entire point is
                           doing both), ControlSequenceOfOperation is
                           CoolingAndHeating, and the hysteresis control
                           loop is reused near-verbatim from firmware/
                           thermostat/'s own (including its default
                           setpoints — 20.00 degC heat / 26.00 degC cool —
                           and 0.3 degC hysteresis band).
                           `HEAT_PUMP_SENSOR_GPIO` reuses the exact DS18B20
                           1-Wire driver this repo's other appliance/HVAC
                           device types already establish verbatim.

                           Output: `HEAT_PUMP_COMPRESSOR_RELAY_GPIO`
                           (active-LOW) runs whenever either heat or cool
                           demand is active — a real heat pump's compressor
                           runs in both modes, only the refrigerant flow
                           direction differs. `HEAT_PUMP_REVERSING_VALVE_
                           RELAY_GPIO` (active-LOW) tracks SystemMode
                           directly (energized only in Cool), independent
                           of the compressor's own on/off cycling — a real
                           reversing valve is pre-positioned for the
                           commanded mode before the compressor ever
                           starts, not toggled per hysteresis cycle. The
                           energized-in-Cool convention matches common "O"
                           terminal wiring, but real heat pump systems are
                           NOT universal here — some use a "B" terminal
                           convention (energized-in-Heat) instead; flagged
                           explicitly, same disclaimer this repo's other
                           relay-polarity choices already carry.
                           Explicitly, deliberately NOT implemented: any
                           defrost cycle or auxiliary/backup electric-heat-
                           strip logic — real cold-climate heat pumps need
                           outdoor coil temperature sensing and real timing
                           logic this hobby-scale single-sensor build
                           doesn't have, same "smallest reasonable next
                           step" scope cut firmware/robot-vacuum/'s own
                           skipped real navigation already establishes.
                           Standard quick-power-cycle factory reset.
                           Build-verified in Docker — clean on the first
                           attempt despite the composition complexity; not
                           hardware-tested (no relay/DS18B20 hardware for
                           this device type physically available when
                           written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/flow-sensor/     Flow Sensor — thirty-fourth device type, back to
                           this repo's simplest recent shape after firmware/
                           heat-pump/'s own composition complexity: Identify
                           + one mandatory cluster, same minimal XML shape
                           firmware/pressure-sensor/'s own device type
                           already established.
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/FlowSensor.xml:
                           Identify + FlowMeasurement are the ONLY clusters
                           listed, both mandatory. `endpoint::flow_sensor::
                           create()` confirmed complete/ready-to-use by
                           reading `esp_matter_endpoint.cpp`'s own
                           `flow_sensor::add()` directly. FlowMeasurement
                           confirmed to be a "code-driven" cluster class
                           (a real `flow_measurement/` folder exists under
                           `data_model_provider/clusters/`), same category
                           as PressureMeasurement/TemperatureMeasurement —
                           `SetMeasuredValue()` via the registry, same
                           pattern. MeasuredValue's own unit (m3/h,
                           resolution 0.1 m3/h) isn't spelled out in
                           Matter's own machine-readable cluster XML (same
                           gap firmware/pressure-sensor/'s own header
                           comment already documents for
                           PressureMeasurement) — confirmed instead against
                           the same real, independent source already used
                           for that file: Home Assistant's own Matter
                           integration (`sensor.py`'s FlowMeasurement
                           discovery schema divides the raw value by 10 and
                           reports m3/h), rather than assumed.
                           `FLOW_SENSOR_PULSE_GPIO` counts rising edges via
                           a GPIO ISR — the same pulse-counting technique
                           firmware/outlet/'s own BL0937/HLW8012/CSE7759
                           power-monitor drivers already establish (a plain
                           `volatile uint32_t` edge counter incremented
                           from an `IRAM_ATTR` ISR, read and reset once per
                           sampling window with no critical section needed).
                           A Hall-effect pulse-output flow sensor
                           (YF-S201-class — no single canonical datasheet,
                           a widely cloned design, same "best available,
                           cross-checked across multiple independent
                           sources" standard already used in this repo for
                           e.g. the contact sensor's reed switch) — the
                           pulse characteristic `F(Hz) = 7.5 * Q(L/min)`
                           and its 1-30 L/min rated range are both
                           consistently documented across every independent
                           source checked. Zero pulses in a window reports
                           a real MeasuredValue of 0 (no flow) rather than
                           null — unlike a bus-based sensor, a passive
                           pulse GPIO has no way to distinguish "sensor
                           absent" from "genuinely zero flow". Min/
                           MaxMeasuredValue (1/18, i.e. 0.1/1.8 m3/h) come
                           directly from the sensor's own rated 1-30 L/min
                           range — same "use real hardware limits for
                           Min/Max" precedent firmware/pressure-sensor/'s
                           own BMP280 operating-range bounds already
                           establish. Standard quick-power-cycle factory
                           reset. Build-verified in Docker (clean first
                           attempt); not hardware-tested (no YF-S201-class
                           sensor physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/humidity-sensor/  Humidity Sensor — thirty-fifth device type, and a
                           standalone sibling to firmware/temperature-sensor/'s
                           own humidity endpoint — recommended three times
                           across recent AskUserQuestion rounds (firmware/
                           room-air-conditioner/'s, firmware/heat-pump/'s,
                           and firmware/flow-sensor/'s own) before finally
                           being chosen.
  main/app_main.cpp        Confirmed against the CSA's own data_model/1.6/
                           device_types/HumiditySensor.xml: Identify +
                           RelativeHumidityMeasurement are the ONLY
                           clusters, both mandatory, with no optional
                           TemperatureMeasurement slot at all (unlike
                           RoomAirConditioner's own optional Temperature/
                           Humidity pair) — confirmed by reading the XML
                           directly. `endpoint::humidity_sensor::create()`
                           is the exact same top-level helper firmware/
                           temperature-sensor/ already calls for its own
                           second (humidity) endpoint — reused here as the
                           ONLY endpoint on a standalone device.
                           RelativeHumidityMeasurement confirmed to be the
                           same "code-driven" cluster category as
                           TemperatureMeasurement — `SetMeasuredValue()`
                           via the registry, same pattern firmware/
                           temperature-sensor/'s own `update_humidity()`
                           already establishes. `SENSOR_TYPE` reuses the
                           same 6-chip library firmware/temperature-sensor/
                           already established (SHT3x/SHT4x/AHT20/DHT11/
                           DHT22/BME280 — every driver reused verbatim:
                           I2C bus setup, Sensirion/Bosch checksum
                           algorithms, register maps, conversion formulas,
                           DHT11/DHT22's shared bit-banged protocol), minus
                           DS18B20 — which measures temperature only and
                           has no humidity output at all, so there's
                           nothing for a humidity-only device type to read
                           from it; unlike firmware/temperature-sensor/'s
                           own `SENSOR_HAS_HUMIDITY` compile-time escape
                           hatch (which still uses DS18B20 for its primary,
                           mandatory temperature endpoint), that option
                           simply isn't offered here at all. Each driver's
                           own `sensor_read()` still returns a temperature
                           value internally (several of these chips measure
                           both together, in one transaction, inseparably)
                           — never exposed via Matter here, the same "read
                           but unused" pattern firmware/thermostat/'s own
                           local-temperature-sensor reuse already
                           establishes. None of the 6 chips is personally
                           hardware-tested in THIS device type's own
                           firmware (no fresh hardware pass was done for
                           this addition) — flagged accordingly, same
                           standard firmware/temperature-sensor/'s own
                           less-verified chips already carry. Standard
                           quick-power-cycle factory reset. Build-verified
                           in Docker for all 6 `SENSOR_TYPE` values; not
                           hardware-tested.
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/water-freeze-detector/  Water Freeze Detector — thirty-sixth
                           device type, and the closest sibling to
                           firmware/water-leak-detector/ in this repo: same
                           BooleanState cluster, same "true = alarm/
                           problem" StateValue direction, same real
                           esp-matter FeatureMap gap and fix — but a
                           temperature-threshold classifier instead of a
                           debounced digital sensor input.
  main/app_main.cpp        Confirmed directly via esp-matter's own
                           `water_freeze_detector::add()`: identical
                           structure to `water_leak_detector::add()`/
                           `contact_sensor::add()` (Identify + BooleanState
                           + StateChange event, via `common::create<T>()`)
                           — matches the CSA's own data_model/1.6/
                           device_types/WaterFreezeDetector.xml exactly.
                           StateValue confirmed to use the same "true =
                           alarm/problem" direction firmware/
                           water-leak-detector/'s own StateValue already
                           uses (confirmed against Espressif's own
                           `MatterWaterFreezeDetector` Arduino-ESP32 class,
                           whose `setFreeze(bool)` sets true for a detected
                           freeze condition) — boots with StateValue false
                           rather than seeding a real boot-time reading the
                           way firmware/water-leak-detector/'s own GPIO
                           sensor can, since a DS18B20 needs one real
                           conversion cycle first before anything can be
                           honestly reported. The same real, documented
                           esp-matter FeatureMap gap firmware/
                           water-leak-detector/'s own header comment
                           documents in full (`boolean_state::create()`
                           hardcodes FeatureMap to 0, never actually
                           setting the ChangeEvent feature bit this device
                           type's own spec (revision 2) makes mandatory)
                           gets the identical fix: a direct
                           `attribute::update()` on FeatureMap after
                           endpoint creation but before
                           `esp_matter::start()`, safe for the same reason
                           (`boolean_state::event::create_state_change()`
                           fires the StateChange event unconditionally,
                           with no feature-flag gate). Unlike firmware/
                           water-leak-detector/'s own cheap comparator
                           probe module, no equally common, hobby-
                           accessible "freeze switch" module exists to
                           point to with the same confidence (real
                           commercial pipe-freeze alarms mostly use a
                           specialized factory-preset bimetallic
                           thermostat switch) — this file instead reuses
                           the exact DS18B20 1-Wire driver already
                           established across this repo's other appliance/
                           HVAC device types verbatim, paired with a plain
                           adjustable threshold (3.00 degC default, a few
                           degrees above the actual 0 degC freezing point
                           to leave response time) + 0.3 degC hysteresis
                           classifier — same "adjustable threshold, not a
                           calibrated reading" precedent firmware/
                           smoke-co-alarm/'s and firmware/
                           air-quality-sensor/'s own classifiers already
                           establish. Standard quick-power-cycle factory
                           reset. Build-verified in Docker (clean first
                           attempt); not hardware-tested (no DS18B20
                           hardware for this device type physically
                           available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/soil-sensor/     Soil Sensor — thirty-seventh device type, and this
                           repo's first over the Soil Measurement cluster —
                           which hid a genuinely new, more severe class of
                           esp-matter gap than anything found in this repo
                           before: skip the one required app-level call
                           this file makes and the device doesn't misbehave
                           quietly, it hard-crashes at startup.
  main/app_main.cpp        Confirmed against the CSA's own data_model/1.6/
                           device_types/SoilSensor.xml: Identify and
                           SoilMeasurement are mandatory; an optional
                           TemperatureMeasurement cluster is listed but not
                           implemented — real cheap capacitive soil-
                           moisture probes don't carry a temperature
                           element at all, same "smallest reasonable next
                           step" scope cut applied elsewhere.
                           `endpoint::soil_sensor::create()` confirmed
                           complete/ready-to-use by reading
                           `esp_matter_endpoint.cpp`'s own `soil_sensor::
                           add()` directly. `soil_measurement::config_t`
                           is a literally empty `common::config_t`, and
                           `create()` only ever creates a plain, data-less
                           ember-attribute shell for
                           SoilMoistureMeasurementLimits before registering
                           an init callback that fires later, during
                           `esp_matter::start()`. Reading THAT callback's
                           own source directly
                           (`data_model_provider/clusters/soil_measurement/
                           integration.cpp`) is where the real severity
                           shows up: a literal `VerifyOrDieWithMsg
                           (gLimits.find(endpointId) != gLimits.end(),
                           ...)` — if the app doesn't call the free
                           function `SoilMeasurement::
                           SetSoilMoistureLimits()` for this endpoint
                           BEFORE `esp_matter::start()` runs, the device
                           aborts the whole firmware outright, not a
                           silent gap the way every prior FeatureMap-class
                           gap in this repo has been. This file calls it
                           right after building the endpoint, passing a
                           real `Globals::Structs::
                           MeasurementAccuracyStruct::Type` (measurementType
                           = `MeasurementTypeEnum::kSoilMoisture`, confirmed
                           against connectedhomeip's own generated shared
                           Enums.h; min/maxMeasuredValue = 0/100, matching
                           SoilMoistureMeasuredValue's own `percent` type).
                           `SetSoilMoistureMeasuredValue()` is the real
                           setter used to report readings afterward —
                           reached via esp-matter's own ready-made free
                           function rather than this repo's usual
                           registry-lookup-and-cast pattern, the same
                           "esp-matter's own integration.cpp provides a
                           convenience free function" category firmware/
                           air-purifier/'s own `ResourceMonitoring::
                           GetClusterInstance()` already established.
                           Sensor: a cheap capacitive soil-moisture probe
                           (analog output, dry = higher voltage, wet =
                           lower voltage — the opposite direction from a
                           simple resistive probe's own DC behavior), read
                           via the same `esp_adc/adc_oneshot.h` +
                           `esp_adc/adc_cali.h` ADC1 pattern firmware/
                           light-sensor/'s own LDR driver already
                           establishes. Unlike that LDR's real datasheet-
                           grounded characteristic curve, this sensor's
                           voltage-to-moisture mapping is NOT chip-
                           datasheet-driven at all — real-world use
                           universally involves a two-point field
                           calibration (dry air = 0%, submerged in water =
                           100%) instead, so `SOIL_SENSOR_DRY_MV`/
                           `SOIL_SENSOR_WET_MV` are explicitly adjustable
                           placeholder defaults, not a measured
                           calibration — same "adjustable, not a
                           calibrated reading" honesty precedent firmware/
                           smoke-co-alarm/'s and firmware/
                           air-quality-sensor/'s own threshold classifiers
                           already establish. Standard quick-power-cycle
                           factory reset. Build-verified in Docker (clean
                           first attempt); not hardware-tested (no
                           capacitive soil-moisture sensor physically
                           available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/dimmable-plug/   Dimmable Plug-In Unit — thirty-eighth device
                           type, and the natural combination of two device
                           types already in this repo: firmware/outlet/'s
                           own plug-in framing and firmware/
                           dimmable-light/'s own LevelControl/PWM output —
                           reused directly rather than reinvented.
  main/app_main.cpp        `endpoint::dimmable_plug_in_unit::create()`
                           confirmed complete/ready-to-use by reading
                           `esp_matter_endpoint.cpp`'s own
                           `dimmable_plug_in_unit::add()` directly:
                           Identify (with TriggerEffect command) + Groups +
                           OnOff (Lighting feature, On + Toggle commands) +
                           LevelControl (OnOff + Lighting features) +
                           ScenesManagement (with CopyScene/CopyScene-
                           response commands), auto-Descriptor via
                           `common::create<T>()`. Confirmed against the
                           CSA's own data_model/1.6/device_types/
                           DimmablePlug-InUnit.xml: those clusters are
                           exactly what's listed (all mandatory,
                           client-side OccupancySensing optionalConform
                           and not implemented). `config_t` confirmed to
                           simply inherit `on_off_plug_in_unit::config_t`
                           and add `level_control`/`level_control_lighting`
                           fields — the exact same relationship firmware/
                           dimmable-light/'s own `dimmable_light::config_t`
                           has to `on_off_light::config_t`, just for the
                           plug-in-unit family instead of the light family.
                           A genuine physical honesty point, not just a
                           style choice: unlike firmware/outlet/'s own
                           default RELAY output, a relay physically CANNOT
                           dim anything — a real AC-mains dimmable plug
                           needs a TRIAC/phase-control dimmer circuit, real
                           safety-relevant power electronics outside what
                           this repo's "read the datasheet, drive the
                           GPIO" style should attempt without real hardware
                           to validate against (same reasoning firmware/
                           evse/'s own safety note already applies). This
                           file instead drives real PWM via
                           `driver/ledc.h` — the exact same LEDC
                           peripheral/settings firmware/dimmable-light/'s
                           own output already uses — appropriate for
                           either a DC load through a MOSFET, or a real
                           commercial AC dimmer module's own PWM/analog
                           dimming-control input, gating an existing
                           dimmer's own control input rather than
                           attempting to switch mains current directly —
                           same "gate an existing device's own control
                           input" framing firmware/thermostat/'s RELAY
                           output and firmware/evse/'s own relay already
                           establish. LevelControl's CurrentLevel confirmed
                           to be a plain ember attribute here too (no
                           `level_control/` folder under
                           `data_model_provider/clusters/`), same
                           `attribute::PRE_UPDATE` pattern as OnOff.
                           `OnLevel` deliberately left null (via
                           `nullable<uint8_t>()`) rather than a concrete
                           level — reusing firmware/dimmable-light/'s own
                           already-found-and-hardware-confirmed real bug
                           fix (a concrete OnLevel forces CurrentLevel back
                           to that fixed value on every plain OnOff::On,
                           discarding whatever brightness a controller last
                           set). Boots off, same convention every other
                           device type here follows. Standard quick-power-
                           cycle factory reset. Build-verified in Docker
                           (clean first attempt); not hardware-tested (no
                           MOSFET/dimmer-module hardware for this device
                           type physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/rain-sensor/     Rain Sensor — thirty-ninth device type, and the
                           third sibling in this repo's BooleanState family
                           after firmware/water-leak-detector/ and
                           firmware/water-freeze-detector/ — same cluster,
                           same real esp-matter FeatureMap gap and fix, and
                           — confirmed, not assumed — literally the SAME
                           physical sensor module as firmware/
                           water-leak-detector/'s own probe, just mounted
                           and marketed for a different purpose.
  main/app_main.cpp        Confirmed directly via esp-matter's own
                           `rain_sensor::add()`: identical structure to
                           `water_leak_detector::add()`/
                           `water_freeze_detector::add()`/
                           `contact_sensor::add()` (Identify + BooleanState
                           + StateChange event) — matches the CSA's own
                           data_model/1.6/device_types/RainSensor.xml
                           exactly. StateValue confirmed to use "true =
                           rain detected" (confirmed against Espressif's
                           own `MatterRainSensor` Arduino-ESP32 class,
                           `setRain(bool)`) — the same "true = the sensed
                           condition is present" direction firmware/
                           water-leak-detector/'s own StateValue already
                           uses, though rain itself isn't inherently a
                           hazard the way a leak or a freeze is, closer in
                           spirit to OccupancySensing's own true=occupied.
                           The same real, documented esp-matter FeatureMap
                           gap firmware/water-leak-detector/'s and
                           firmware/water-freeze-detector/'s own header
                           comments already document in full
                           (`boolean_state::create()` hardcodes FeatureMap
                           to 0, never setting the ChangeEvent feature bit
                           this device type's own spec makes mandatory)
                           gets the identical fix. The sensor module itself
                           is confirmed — via multiple independent sources,
                           not assumed from the name alone — to be
                           literally the same board hardware as firmware/
                           water-leak-detector/'s own probe (a set of
                           parallel PCB traces feeding an LM393 comparator,
                           DO pin), sold interchangeably as either a
                           "water sensor" or "rain sensor" depending on
                           mounting; the debounce/edge-handling logic (GPIO
                           ISR + FreeRTOS queue, ANYEDGE, ~40ms consistent-
                           level debounce) is reused verbatim from that
                           file's own `water_leak_task()`. Standard
                           quick-power-cycle factory reset. Build-verified
                           in Docker (clean first attempt); not
                           hardware-tested (no rain sensor module
                           physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/color-temperature-light/  Color Temperature Light — fortieth device
                           type, and this repo's first tunable-white bulb:
                           cool-white + warm-white channels only, no RGB at
                           all, matching real "tunable white"/"CCT" smart-
                           bulb products.
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/
                           ColorTemperatureLight.xml (device type 0x010C,
                           classification "superset: Dimmable Light"):
                           Identify + Groups + OnOff[Lighting] +
                           LevelControl[OnOff+Lighting] + ScenesManagement +
                           ColorControl are all mandatoryConform, but
                           ColorControl only requires its ColorTemperature
                           (CT) feature — HueSaturation and XY are not part
                           of this device type's cluster requirements at
                           all, unlike ExtendedColorLight (0x010D, what
                           firmware/color-light/ and firmware/
                           addressable-light/ both implement), which
                           mandates all three color features. This is the
                           correct, spec-conformant device type for
                           hardware that genuinely only does cool/warm
                           white blending, rather than declaring
                           ExtendedColorLight and only half-implementing
                           its mandatory color surface.
                           `endpoint::color_temperature_light::create()`
                           confirmed to be a COMPLETE top-level helper by
                           reading esp-matter's own legacy
                           `color_temperature_light::add()` directly —
                           unlike firmware/color-light/'s and firmware/
                           addressable-light/'s own hand-assembled
                           ExtendedColorLight endpoints (built that way
                           specifically to get HueSaturation, which
                           esp-matter's own extended_color_light helper
                           never wires up), this device type's helper
                           already builds exactly what the CSA XML
                           requires — including the mandatory RemainingTime
                           attribute — and, via the same shared
                           `common::create<T>()` template every complete
                           top-level helper in this repo uses, creates the
                           endpoint's Descriptor cluster automatically too.
                           This sidesteps BOTH real bugs found and fixed on
                           color-light's/addressable-light's own hand-
                           assembled endpoints during real Apple Home
                           hardware testing (missing Descriptor cluster;
                           missing mandatory ColorControl features/
                           attributes — see CLAUDE.md's "Open next steps"
                           for the full debugging story) — nothing to work
                           around here. Also, unlike those two files' own
                           RGBWW/RGBCCT modes, no color-space interlock is
                           needed: this device type has exactly one color
                           feature (ColorTemperature), so there is nothing
                           to interlock between — ColorTemperatureMireds is
                           the only color input, always rendered straight
                           to the cool/warm channels.
                           OnOff/LevelControl/ColorTemperatureMireds all
                           confirmed to be plain ember attributes (no
                           `color_control/` or `level_control/` folder
                           under `data_model_provider/clusters/`, the same
                           check every prior light device type in this
                           repo already applies) — same `attribute::
                           PRE_UPDATE` + `attribute::update()` pattern as
                           firmware/dimmable-light/'s and firmware/
                           color-light/'s own outputs. `mireds_to_cw_ww()`
                           is copied unchanged from firmware/color-light/'s
                           own RGBWW-mode function of the same name (itself
                           ported from ESPHome's real `light_call.cpp`) —
                           clamp into range, linearly interpolate the warm
                           fraction, then normalize both fractions by
                           whichever is larger so at least one channel
                           always reaches full strength at any color
                           temperature. COLOR_TEMPERATURE_LIGHT_COOL_WHITE_
                           KELVIN/_WARM_WHITE_KELVIN default to 6500K/2700K,
                           the same two most common LED bin ratings
                           firmware/color-light/'s own header comment
                           already documents — explicitly adjustable per
                           your actual LEDs' rated color temperature.
                           OnLevel is left null (`nullable<uint8_t>()`),
                           reusing the same real, hardware-confirmed bug
                           fix firmware/dimmable-light/'s and firmware/
                           color-light/'s own LevelControl configs already
                           apply (a concrete OnLevel would force
                           CurrentLevel back to a fixed value on every
                           plain OnOff::On instead of restoring the last
                           brightness). Two LEDC PWM channels (cool white
                           on GPIO 2, warm white on GPIO 4 — the same
                           "GPIO2 first output, GPIO4 second" pattern
                           firmware/dimmable-light/ already establishes for
                           a single channel, extended to two here), plus an
                           Identify LED on GPIO 15 (same default firmware/
                           color-light/'s own identify LED already uses).
                           Boots off, same convention every device type in
                           this repo follows. Standard quick-power-cycle
                           factory reset. Build-verified in Docker (one
                           real, quickly-caught compile error: `fminf`/
                           `fmaxf` need `<cmath>`, not pulled in
                           transitively the way it apparently is in
                           firmware/color-light/ — fixed by adding the
                           include directly); not hardware-tested (no
                           cool-white/warm-white LED/driver board for this
                           device type physically available when written).
  partitions.csv           same OTA + fctry layout as firmware/light/
  sdkconfig.defaults        same as firmware/light/
firmware/closure/         Closure (garage door / roller shutter / awning) —
                           forty-first device type, and this repo's first
                           over the Closure Control cluster — a brand-new
                           (Matter 1.6) cluster family, not yet used
                           anywhere else in this repo.
  main/app_main.cpp        Confirmed directly against the CSA's own
                           data_model/1.6/device_types/Closure.xml (device
                           type 0x0230): Identify + Closure Control are the
                           only two clusters, both mandatoryConform — Window
                           Covering and Closure Dimension are both
                           explicitly `<disallowConform/>` on this device
                           type (they belong to the separate ClosurePanel
                           child-endpoint device type, 0x0231, for closures
                           with more than one independently-controlled
                           panel — not implemented here; single-panel is the
                           common garage-door/roller-shutter/awning case).
                           `endpoint::closure::create()` confirmed complete/
                           ready-to-use (Identify + ClosureControl, auto-
                           Descriptor via `common::create<T>()`) by reading
                           esp-matter's own legacy `closure::add()`
                           directly. ClosureControl's own cluster XML
                           defines nine optional features; Positioning (PS)
                           and MotionLatching (LT) form a real "at least one
                           of these two" choice group — confirmed both in
                           the XML's own conform markers and in esp-matter's
                           own `closure_control::create()`, which calls
                           `VALIDATE_FEATURES_AT_LEAST_ONE("Positioning,
                           MotionLatching", ...)`, the same "choice, at
                           least 1" constraint class firmware/
                           occupancy-sensor/'s OccupancySensing already
                           established. This file enables Positioning only
                           — same "smallest reasonable next step" scoping
                           firmware/window-covering/'s own Lift-only choice
                           already applies — mapping directly onto a simple
                           two-relay (open/close) motor with no position
                           sensor: FullyClosed/FullyOpened/PartiallyOpened
                           are the only CurrentPositionEnum values ever
                           reported (OpenedForPedestrian/OpenedForVentilation
                           need PD/VT, not enabled; OpenedAtSignature is
                           spec-mandatory with no feature gate at all, but
                           has no real meaning on this hardware, so it's
                           simply never used).

                           A genuinely new, eighth "how do I wire up a
                           code-driven cluster's real implementation from
                           app code" pattern for this repo (after: plain
                           registry-lookup setter; a Delegate whose own
                           reporting call happens to be a working generic
                           free-function proxy; the `chip::app::…
                           registry().Get()`-based fallback for
                           `DefaultServerCluster`-derived clusters; a
                           cluster-family-specific convenience free
                           function; a direct FeatureMap `attribute::
                           update()` override; `get_delegate_managed_
                           instance()` for a legacy-ember-style cluster with
                           a delegate-managed live instance; and a
                           `SetDefaultDelegate()`/`SetDelegate()`-style free
                           function/method called AFTER `esp_matter::
                           start()`) — and, unlike all seven of those, the
                           OPPOSITE ordering: ClosureControl's delegate must
                           be registered BEFORE `esp_matter::start()`.
                           Confirmed by reading BOTH esp-matter's own
                           `data_model_provider/clusters/closure_control/
                           integration.cpp` AND connectedhomeip's own
                           `ClosureControlCluster::Config` constructor
                           directly: `Config` takes
                           `ClosureControlClusterDelegate & delegate` as a
                           mandatory reference, not an optional pointer
                           supplied later, so the cluster object cannot be
                           constructed without one — and esp-matter's own
                           `ESPMatterClosureControlClusterServerInitCallback`
                           (registered as this cluster's plain
                           `init_callback`, run by `invoke_init_callbacks_
                           internal()` as part of `esp_matter::start()`,
                           BEFORE that same per-cluster pass's own
                           `delegate_init_callback` step that
                           `config->closure_control.delegate` would
                           otherwise wire up automatically) refuses to
                           construct the cluster at all if no delegate has
                           been registered for that endpoint yet — its own
                           source literally logs "delegate not set for ep
                           %u (call MatterClosureControlSetDelegate first)"
                           and returns. `config->closure_control.delegate`
                           is therefore left null here; the real delegate
                           is registered explicitly via `chip::app::
                           Clusters::ClosureControl::
                           MatterClosureControlSetDelegate(endpoint_id,
                           delegate)` (declared in `data_model_provider/
                           clusters/closure_control/integration.h`) BEFORE
                           the `esp_matter::start()` call in `app_main()` —
                           the opposite placement from firmware/valve/'s
                           and firmware/fan/'s own delegate registration.
                           This finding is sourced entirely from reading
                           esp-matter's/connectedhomeip's own code
                           directly, same discipline as every other
                           runtime-ordering conclusion in this repo, but —
                           like firmware/fan/'s own `SetDefaultDelegate()`
                           ordering bug before it was hardware-confirmed —
                           has not itself been confirmed against real
                           runtime behavior yet (no hardware for this
                           device type was available when written).

                           What the cluster does automatically vs. what
                           this file does, confirmed by reading
                           `ClosureControlCluster.cpp`'s own `HandleMoveTo()`/
                           `HandleStop()`/`HandleCalibrate()`/`SetMainState()`
                           directly: the cluster validates commands against
                           MainState/FeatureMap conformance, calls the
                           delegate, and — only on success — sets MainState
                           and OverallTargetState itself, plus pulls
                           `GetMovingCountdownTime()`/etc. from the delegate
                           to republish CountdownTime and generates the
                           mandatory EngageStateChanged event on any
                           Disengaged transition (never reached here). What
                           it does NOT do automatically — same "the app
                           should trigger the state change" responsibility
                           firmware/door-lock/'s LockState and firmware/
                           valve/'s CurrentState already establish — is
                           anything about the *ongoing* physical movement:
                           reporting live OverallCurrentState/SecureState as
                           the motor travels, deciding when travel is
                           complete, transitioning MainState back to
                           Stopped, and firing MovementCompleted/
                           SecureStateChanged. All of that is this file's
                           own `closure_task`, built on the same time-based
                           position-estimation technique (no position
                           sensor assumed) firmware/window-covering/'s own
                           `movement_task` already established — the
                           difference here is that ClosureControl only
                           exposes three coarse discrete position states,
                           not a continuous percentage attribute, so this
                           file estimates a continuous 0-100 "percent open"
                           value purely internally (for arrival detection
                           and the CountdownTime countdown) and only ever
                           reports the three coarse enum states a
                           controller can actually see. SecureState is this
                           file's own reasonable, documented interpretation
                           — ClosureControl is new enough (Matter 1.6) that
                           no equivalent to e.g. Espressif's own
                           `MatterWaterLeakDetector` Arduino-ESP32 class
                           exists yet to confirm a "true" direction against
                           the way firmware/water-leak-detector/'s and
                           firmware/rain-sensor/'s own StateValue directions
                           were: true (securing against unauthorized entry)
                           exactly when FullyClosed, false otherwise — the
                           natural reading of the spec's own field
                           description, not a confirmed universal
                           convention. `SetOverallCurrentState()`'s first
                           call (seeding a real initial state, required
                           before `HandleMoveTo()` will accept any MoveTo
                           command at all — confirmed by reading it
                           directly) happens right after `esp_matter::
                           start()` returns, via the same registry-lookup-
                           and-cast pattern (`chip::app::…registry().Get()`
                           + `static_cast<ClosureControlCluster*>`)
                           firmware/valve/'s own `get_valve_cluster()`
                           already establishes, since ClosureControl has no
                           `GetClusterInstance()`-style convenience free
                           function. Boots assumed FullyClosed (0% open) —
                           the safer default for something whose whole
                           purpose is "closure", matching firmware/valve/'s
                           own boot-closed convention; an assumption, not a
                           measurement, until the first real movement, same
                           caveat firmware/window-covering/'s own boot-open
                           assumption already carries. Two relay outputs
                           (open/close, active-LOW), same GPIO 4/5 defaults
                           firmware/window-covering/'s own two-relay output
                           already uses — same hardware shape, different
                           device type. Standard quick-power-cycle factory
                           reset. Build-verified in Docker (clean first
                           attempt); not hardware-tested (no garage-door/
                           roller-shutter motor+relay hardware for this
                           device type physically available when written).
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
   rather than added and silently broken. Also: each device-type card on
   Get Started gained its own hand-drawn line-art icon
   (`DEVICE_TYPE_ICONS`), styled after Apple's own SF Symbols/HomeKit
   icons — refined after a first pass (thinner strokes, no redundant
   frame around the icon since the card button already has one) per
   feedback, then caught and fixed a real problem by actually rendering
   the page with a headless Chromium (installed specifically for this)
   rather than just reading the SVG markup: the outlet icon (bare
   circle + 2 dots) read as a smiley face once rendered at real card
   size, so it got its wall-plate square frame back as the one
   deliberate exception. Screenshot-checking wizard changes this way —
   not just the Node.js harness's structural smoke tests — is worth
   doing for any future visual-design change to this file.

   A ninth device type, `firmware/color-light/` (still declared as the
   `ExtendedColorLight` device type, 0x010D), followed — the last of
   the three options offered together back when dimmable light was
   chosen (dimmable light and window covering were the other two, both
   already built). Implements exactly one ColorControl mode
   (Hue/Saturation) rather than esp-matter's own
   `endpoint::extended_color_light::create()` default of Xy +
   ColorTemperature (confirmed in source that helper never actually
   wires up HueSaturation at all, despite that being what most
   controllers' color wheels drive first) — same "smallest reasonable
   next step" scoping as dimmable-light/window-covering, avoiding two
   more real colorimetry conversions (CIE xyY→RGB,
   correlated-color-temperature→RGB) this session judged out of scope.
   Built by calling esp-matter's own lower-level free functions directly
   rather than the higher-level endpoint helper — the first device type
   in this repo assembled that way. One real compile error (a namespace-
   qualification slip) was caught by an actual Docker build, fixed on
   the second attempt. Build-verified in Docker; not hardware-tested (no
   RGB LED/driver board physically available when written). See its own
   repository-layout entry above for the full detail.

   Building this device type also surfaced and fixed a real, previously
   latent bug in the wizard's `extraButtons` mechanism: it was designed
   only for switch's *variable* 1-4 button count, and assumed that shape
   everywhere (a "how many?" picker, a `buttonCountDefineName` always
   present, hardcoded "BUTTON N" labels). Color-light's Green/Blue
   channels needed the same array-of-extra-GPIO-fields shape but as a
   *fixed* set — always all three, no picker, no count `#define`. Fixed
   by adding a `hasVariableButtonCount` check (true only when
   `buttonCountDefineName` is set) that gates the picker/count-sed logic,
   plus per-entry `label` fields (and a device-type-level `driverLabel`)
   so summary rows say "RED CHANNEL" instead of the switch-specific
   "BUTTON 1" fallback. This also caught a second, independently real
   bug while fixing the first: `isProductComplete()` computed its own
   `buttonCount` separately from `renderConfigureDevice()`'s, defaulting
   to 1 instead of the fixed device type's true count whenever called
   before Configure Device had rendered even once — meaning a
   fixed-3-field product could have been reported "complete" with only
   1 of 3 required fields actually set. Fixed the same way in both
   places, and added a smoke test that calls `isProductComplete()`
   first, with no prior render, specifically to catch a regression here.

   `firmware/color-light/` was then extended with an optional RGBW mode
   (`COLOR_LIGHT_HAS_WHITE_CHANNEL`, off by default) — a 4th LEDC
   channel, with the same Hue/Saturation color converted to RGBW via the
   standard "extract common white" technique (matches Home Assistant's
   own color utility and WLED, not invented for this file). The wizard
   integration needed zero new mechanism: RGB-vs-RGBW turned out to fit
   the *existing* Power Monitoring picker's `pins`-per-component
   mechanism (built for `firmware/outlet/`) exactly — a build-time
   hardware choice with its own `#define` plus one extra GPIO when
   picked, same shape as choosing a power-monitor chip. Build-verified
   in Docker for both RGB and RGBW; not hardware-tested. Screenshot-
   verified in the wizard too (both the plain RGB card grid and the
   RGBW mode's White Channel field rendering correctly), continuing the
   "actually render wizard changes with a headless Chromium" practice
   started when the outlet icon issue was caught.

   `firmware/color-light/` then gained a third variant, RGBWW (what LED
   strip vendors sell as "RGBCCT"/"RGB+CCT" — separate cool-white and
   warm-white channels instead of RGBW's single white channel), prompted
   directly by the user listing out real product categories (RGB, RGBW,
   RGBIC, RGBWW, RGBCCT) they wanted covered. Unlike RGBW, this isn't
   just "one more optional channel": real RGBCCT hardware doesn't blend
   RGB and white simultaneously (confirmed via ESPHome's own rgbww light
   component docs — "color_interlock", "it is not possible to enable the
   RGB leds at the same time as the white leds" on this class of
   hardware), which maps directly onto Matter's ColorControl cluster
   already distinguishing Hue/Saturation from ColorTemperatureMireds as
   separate `ColorMode` values — so RGBWW mode adds the ColorTemperature
   feature (confirmed field-by-field against esp-matter's own
   color_control.h: `color_temperature_mireds`,
   `color_temp_physical_min/max_mireds`,
   `couple_color_temp_to_level_min_mireds`,
   `start_up_color_temperature_mireds`) and the firmware locally latches
   which color space was most recently commanded, driving either the RGB
   channels or the cool/warm channels — same interlock effect as
   ESPHome's, against Matter's own cluster. The mireds→channel-duty
   conversion reuses ESPHome's own `light_call.cpp`
   `transform_parameters_` formula verbatim (fetched and read directly,
   not assumed): clamp into range, linear-interpolate the warm/cool
   fraction, then normalize both by their max so at least one channel
   stays at full strength at any color temperature instead of both
   dimming together at the midpoint. This forced `COLOR_LIGHT_COLOR_MODE`
   from a boolean flag into a real 3-way enum
   (`COLOR_LIGHT_MODE_RGB`/`_RGBW`/`_RGBWW`) — safe to do since the
   RGBW-only version had only just been committed, not yet released or
   hardware-tested by anyone. The wizard side needed zero new
   render/validation/sed logic despite this: RGBWW's 2 extra GPIO fields
   (cool white, warm white) go through the exact same multi-pin `pins`
   array mechanism `firmware/outlet/`'s BL0942 (2 UART pins) and ADE7953
   (SDA/SCL) options already exercise — confirmed by actually reading
   that render/validation code path before assuming it only ever handled
   one pin per option, rather than guessing. Build-verified in Docker for
   all three color modes; not hardware-tested (no RGB(W)(W) LED/driver
   board physically available when written).

   A tenth device type, `firmware/addressable-light/`, followed directly
   from the user's request to add addressable LED strip support (WS2812B,
   SK6812, "and other common ones") after RGBWW shipped — a genuinely
   different technology from every prior light in this repo (single-wire
   NRZ protocol via the RMT peripheral, not PWM), so it got its own
   folder rather than becoming another firmware/color-light/ mode. Before
   writing any driver code, checked what "addressable" could actually
   mean over Matter: connectedhomeip's own
   `controller-clusters.matter` does define a `DynamicLighting` cluster
   (0x0305, EffectStruct/EffectColorStruct — exactly the shape a real
   per-pixel/gradient effect would need) but it's marked `provisional`
   and absent from every ratified data_model spec folder (checked 1.0
   through 1.6) — not usable against any real, certified controller
   today. So this device type does exactly what firmware/color-light/
   does (one Hue/Saturation/Level color for the whole accessory) over a
   different physical layer — explicitly documented as NOT "RGBIC"
   per-zone control, to avoid over-promising what Matter can actually
   drive. `#define ADDRESSABLE_LIGHT_CHIP` selects WS2812B (default,
   24-bit/3 bytes per pixel, GRB order) or SK6812 (32-bit RGBW/4 bytes
   per pixel, RGBW order) — both independently verified against
   Worldsemi's own datasheets (fetched as PDFs and read via `pdftotext`
   rather than trusted from search-engine summaries, which turned up two
   conflicting "official" WS2812B timing tables in the process — a
   useful reminder that even "primary source" web results need the
   actual document read, not just a snippet). SK6812's RGBW byte order
   is flagged in a code comment as a real point of disagreement with
   several community Arduino/ESPHome libraries (which default to GRBW
   instead) — if a real strip's colors come out swapped, that's the
   first thing to check. Implemented via ESP-IDF's `driver/rmt_tx.h` —
   this repo's first RMT-based driver (every prior timing-sensitive
   driver, e.g. DHT11/DHT22/DS18B20, bit-bangs a GPIO instead) — with the
   exact API pattern checked against Espressif's own official
   `examples/peripherals/rmt/led_strip_simple_encoder` reference (which
   lists classic ESP32 among its supported targets), using only that
   example's API shape, not its timing numbers (those come from the
   datasheets). Wizard integration needed zero new mechanism: the chip
   choice reuses the same componentOptions/componentDefineName pattern
   the temperature/light sensors already use. Build-verified in Docker
   for both chips; not hardware-tested (no WS2812B/SK6812 strip
   physically available when written). See its own repository-layout
   entry above for the full detail, including the exact datasheet-sourced
   timing constants and the reasoning for a deliberately generous
   WS2812B reset time.

   `firmware/addressable-light/`'s pixel count
   (`ADDRESSABLE_LIGHT_PIXEL_COUNT`) was then made wizard-configurable, on
   request, immediately after the device type itself shipped — it had
   been left as a hand-edit-only `#define` because the wizard had no
   field type for a plain integer, only GPIO pins and named enum
   choices. Rather than a one-off addressable-light-specific hack, this
   became a new generic `numberField` mechanism on `DEVICE_TYPES`
   entries (`key`/`label`/`fieldLabel`/`blockTitle`/`filePath`/
   `defineName`/`defaultValue`/`min`/`max`/`helpText`), following the
   same "build the general case, not the specific one" precedent as
   `extraPickers`/`componentOptions`/`hasVariableButtonCount` before it
   — reusable by any future device type that needs a plain numeric
   `#define` (a poll interval, a channel count, etc.), not just this one.
   Touched every layer a GPIO field already does —
   `renderConfigureDevice` (render + validate + default-fill),
   `isProductComplete` (validated independently of any prior render, same
   defensive pattern as `hasVariableButtonCount`'s fix), `buildSedCommands`
   (broad `.*` pattern, since there's no `GPIO_NUM_` prefix to match),
   `renderCustomiseReview`'s summary row, and the device-type-switch
   reset logic (looped over every device type's `numberField`, same
   forward-compatible shape as the existing `extraPickers` reset loop) —
   plus a new delegated `data-number-field` input handler, mirroring
   `data-pin-define`'s existing shape but writing directly to
   `state.currentProduct[key]` instead of a nested `pickerPins` object,
   since a `numberField` is a single top-level value, not one of several
   pins on a chip choice. Verified with a Node.js sandboxed re-exec (the
   `isProductComplete`-before-any-render regression check, an
   out-of-range value, and the exact generated sed command) and a
   headless-Chromium screenshot across three states (default value,
   an invalid value showing the field-level error and disabling "Next
   step", and the Customise & Review step showing both the summary row
   and the correct sed command) — the sandboxed HTML-content assertions
   initially all failed for an unrelated reason (the test harness's
   fake `document.createElement` stub didn't implement `textContent`/
   `innerHTML`, so every `escapeHtml()` call returned `undefined`
   inside the sandbox only) — worth remembering as a harness pitfall,
   not a wizard bug, next time a Node sandbox check inspects rendered
   HTML content rather than just data structures.

   `firmware/addressable-light/` then grew from 2 chips to 8, on request
   (WS2812B, SK6812, SK6812 RGBW, WS2813, WS2815, APA102, then — mid-turn,
   after the user shared screenshots of a real manufacturing/config tool's
   "Device Drivers"/"Indicators" screens — WS2805 and SM2335EGH too). Two
   real scope questions came up before any of this was built, both
   resolved via AskUserQuestion rather than assumed: whether the same chip
   list should also appear on `firmware/color-light/` (Color Light
   physically cannot drive any addressable protocol — plain PWM only — so
   the user chose to add them there purely as a cosmetic reference list,
   not real functionality), and whether "5ch" should be covered by a real
   chip (none of the first 6 requested chips is actually 5-channel — the
   user chose to have a genuine one researched, which led to WS2805).
   WS2813/WS2815's own datasheets turned out to cite timing windows that
   do NOT simply contain WS2812B's values (WS2812B's own 400ns T0H sits
   just outside WS2813/WS2815's cited 220-380ns range) — caught by
   actually checking the numeric ranges before assuming
   WS2812B-compatibility, a real near-mistake worth remembering: chip
   "family" reputation isn't the same as verified numeric compatibility.
   APA102 needed this repo's first real SPI driver (`driver/spi_master.h`)
   and its first genuinely different reverse-engineered-source approach
   for a chip with no usable official datasheet at all (APA102's own is
   too thin on protocol detail to use) — followed cpldcpu.com's
   independently-verified writeup instead, cross-checked against
   Adafruit's/SparkFun's own guides. SM2335EGH turned out to be
   architecturally different from every pixel-chain chip here — a
   single-fixture RGBCCT smart-bulb driver, not an addressable strip at
   all — confirmed by fetching its manufacturer's own "datasheet" (a
   one-page feature summary with zero protocol detail) and finding that
   ESPHome's own driver authors had the identical experience asking for a
   real one; this file's SM2335EGH protocol implementation is therefore a
   verbatim port of ESPHome's own open-source, hardware-tested
   `sm10bit_base.cpp`, the best available source given no real datasheet
   exists. Because SM2335EGH has no pixel-chain concept, the wizard's
   pixel-count field needed a genuine "some chips don't use a field this
   device type otherwise has" mechanism (`hidesNumberField`) — while
   building that, two independent, real, pre-existing gaps were caught
   and fixed: `renderCustomiseReview`'s secondary-pin review row never
   checked `usesPin2` at all (would have shown a misleading Clock-pin row
   for chips that don't use one), and the sidebar's "verified" framing
   would have called a purely cosmetic chip choice "not personally
   tested... build-verified only" — technically true but actively
   misleading, since nothing is ever built from that choice. On top of
   all this, per the user's explicit instruction, Identify now defaults
   to the SAME GPIO as the data pin (flashing the whole strip/fixture
   instead of a separate LED) — implemented carefully, not naively:
   configuring a second plain GPIO on a pin already owned by an RMT
   channel, SPI bus, or bit-banged protocol would corrupt that
   peripheral's output, so this is a runtime check
   (`identify_via_strip`) rather than the naive `#if GPIO_NUM_2 ==
   GPIO_NUM_15`-style comparison, which would silently and incorrectly
   evaluate true for any two GPIO values (GPIO_NUM_* are C enum
   constants, not preprocessor macros, so the preprocessor treats both
   sides as undefined-identifier-equals-0) — a real class of bug worth
   remembering for any future GPIO-equality check written as `#if` in
   this repo. Build-verified in Docker for all 8 chips; not
   hardware-tested (none of the 8 chips' hardware was physically
   available when written). The richer Indicator/Identify-effect state
   machine visible in those same manufacturing-tool screenshots (Setup
   mode/started/complete/failed, Identification blink/breathe/okay/
   channel-change/...) was deliberately deferred as a separate, later
   task — it's a cross-cutting change touching every device type's
   `app_identification_cb`, not just this one, and was explicitly scoped
   out of this same sitting to keep this chip-expansion change reviewable
   on its own.

   An eleventh device type, `firmware/thermostat/` (Thermostat, Heat +
   Cool), followed the RGB Status LED (later removed) / Factory Reset
   cross-cutting work below — this repo's first device type with a
   genuine control loop
   (compares a measured value against a setpoint and drives an output)
   rather than a direct command pass-through or plain sensor readout.
   Requested with real ambition from the start: Heat + Cool (not
   Heat-only, the initially-offered smaller scope), specifically usable
   with European heating boilers via three genuinely different output
   mechanisms requested together — direct relay wiring (a boiler's
   standard T1-T2 volt-free contact input), a bound remote relay module
   (Matter's own Binding cluster, reusing firmware/switch/'s existing
   client-invoke pattern), and a full native OpenTherm master — plus,
   mid-session, an optional rotary encoder and a choice of three local
   displays (GC9A01 round TFT, ST7789 bar TFT, SSD1306 OLED). See the
   device type's own repository-layout entry above for the complete
   technical detail; the headline points: `endpoint::thermostat::create()`
   turned out to be a complete, directly usable esp-matter helper (unlike
   color-light's extended_color_light gap); the OpenTherm protocol was
   verified against the OpenTherm Association's own Protocol Specification
   v2.2 PDF (fetched and read via pdftotext) with the GPIO-level driver
   logic ported from Ihor Melnyk's real, widely-used opentherm_library;
   and a genuine Docker-build-caught IRAM overflow (68 bytes, from the SPI
   display driver on top of everything else this device type already
   does) was fixed via a real ESP-IDF Kconfig option
   (`CONFIG_SPI_MASTER_ISR_IN_IRAM=n`) rather than cutting scope. Wizard
   integration surfaced one real, previously-latent structural gap:
   `renderConfigureDevice`'s left-sidebar rendering assumed a sensor-model
   picker and `extraPickers` were mutually exclusive (true for every
   device type until this one, which needs both at once) — fixed by
   letting them coexist in the same sidebar. All three output modes, the
   rotary encoder, and all three displays are Docker build-verified
   across every combination tested; none of this device type's hardware
   (an OpenTherm adapter board, a rotary encoder, or any of the three
   displays) was physically available when written, so nothing here is
   hardware-tested yet — the first genuinely large gap between
   build-verified and hardware-verified for a device type in this repo,
   worth closing before treating it as done the way the other ten are.

   A twelfth device type, `firmware/camera/` (Matter Camera), followed
   immediately — chosen deliberately as a real stress test of this
   repo's own conventions rather than something scoped down to fit them.
   Real Matter Camera (`WebRTCTransportProvider` +
   `CameraAvStreamManagement`, live WebRTC video) needs simultaneous
   Matter signaling and real hardware video encoding — more than any
   single chip in this repo's existing module list can do, so this is a
   verbatim copy of esp-matter's own reference `examples/camera`
   (Public Domain/CC0), not a rewrite: reimplementing ~5,300 lines of
   production WebRTC/Matter integration from scratch would be both
   infeasible and strictly worse than reusing Espressif's own tested
   code, the same reasoning already applied at smaller scale to
   SM2335EGH/APA102/OpenTherm. Before committing to this, the user was
   asked directly (three real options: build the full ESP32-P4+C6
   dual-chip version matching Espressif's own architecture; build a
   much simpler non-Matter-compliant "best-effort" JPEG-over-HTTP camera
   on an ordinary single ESP32 instead; or skip camera entirely for a
   device type that actually fits this repo's existing single-chip
   model) — the user chose the full, real, Matter-compliant version,
   knowingly accepting everything that implies. This is the first
   device type in this repo that doesn't fit its own established "one
   ESP32 chip, one self-contained firmware image, no external SDKs"
   pattern at all: it's a two-chip split architecture (ESP32-P4 for
   camera + hardware video encode, ESP32-C6 for Wi-Fi/BLE + Matter, one
   physical **ESP32-P4 Function EV Board**, talking over SDIO), of which
   `firmware/camera/` is only the ESP32-C6 signaling half — the ESP32-P4
   media half is the Amazon Kinesis Video Streams WebRTC SDK's own
   `streaming_only` example, not part of this repo — and it needs that
   external SDK (cloned separately, `beta-reference-esp-port` branch,
   with its own five submodules) rather than only the pinned Docker
   image everything else here needs. Actually, genuinely
   Docker-build-verified rather than assumed to work: the real SDK was
   cloned (shallow, `--depth 1`, matching Espressif's own instructions),
   its submodules initialized, and `idf.py build` for `esp32c6` run
   against it inside the pinned `espressif/esp-matter:
   release-v1.6_idf_v5.5.4` image — succeeded. Not hardware-tested (no
   ESP32-P4 Function EV Board was physically available), and
   deliberately not offered in `tools/product-wizard/` at all — its
   one-chip-one-firmware-one-board data model has no way to represent a
   two-chip/two-firmware/external-SDK device honestly. See
   firmware/camera/README.md's own preamble (this repo's own addition,
   ahead of Espressif's unmodified original README) and CLAUDE.md's own
   repository-layout entry above for the complete detail.

   A thirteenth device type, `firmware/door-lock/` (Matter Door Lock),
   followed camera — back to this repo's normal one-chip/one-firmware
   pattern after camera's deliberate exception. The user chose "Door
   Lock" from a short AskUserQuestion list (Door Lock / Smoke-CO Alarm /
   Occupancy Sensor / other). This is the first device type in this repo
   where the main command (LockDoor/UnlockDoor) is handled through a
   plain C weak-symbol override rather than either the
   `attribute::PRE_UPDATE` pattern (OnOff/LevelControl/ColorControl/
   Thermostat elsewhere) or a C++ Delegate class (WindowCovering,
   WebRTCTransportProvider in firmware/camera/) — traced directly from
   connectedhomeip's own `HandleRemoteLockOperation()` source and
   `door-lock-server.h`'s own doc comment ("should be implemented by the
   server app") before writing any command-handling code, confirming
   this is the SDK's documented extension point for an app with no
   Delegate configured, not a workaround. Two real, separate build
   failures were caught by actual Docker builds, not by inspection: (1) a
   compile error — `chip::app::Clusters::DoorLock::DlLockType` needs full
   qualification, since door-lock-server.h's own top-level `using`
   declarations pull in `DlLockState`/`DlStatus`/several other DoorLock
   enums unqualified but NOT `DlLockType`; (2) a linker error —
   `emberAfDoorLockClusterInitCallback` has a plain, non-weak prototype
   (unlike the two command callbacks, which the SDK stubs out with a
   default `return false` when unimplemented), so leaving it undefined is
   a hard `undefined reference`, not a silent default; fixed by adding a
   one-line definition calling
   `DoorLockServer::Instance().InitServer(endpoint)`, confirmed against
   the SDK's own `examples/door_lock/main/lock/door_lock_callbacks.cpp`
   reference. `DOOR_LOCK_OUTPUT_TYPE` offers SERVO (default — a hobby
   servo retrofitting an existing thumb-turn deadbolt, the same approach
   countless DIY/ESPHome smart-lock projects use) or RELAY (an electric
   strike/solenoid, matching firmware/outlet/'s own active-LOW
   convention); an optional position sensor (reed switch,
   `firmware/contact-sensor/`'s own simple digital-input technique) lets
   LockState reflect a real reading instead of the spec-allowed
   optimistic default. Standard factory reset. The
   wizard integration reused every existing mechanism (`extraPickers` for
   Output type, the `driver`/`identify`/`statusLed` shapes already
   established) and added exactly one new parallel field,
   `positionSensor` — a deliberate duplicate of `statusLed`'s single-GPIO
   checkbox-gated shape rather than a reuse of that literal field, since
   `statusLed`'s hardcoded "Add a Status LED" UI text would misdescribe a
   sensor input as an LED output. Build-verified in Docker across all 3
   meaningful configs (servo/no-sensor, servo/with-sensor,
   relay/no-sensor); not hardware-tested (no servo/relay/reed-switch
   hardware for this device type physically available when written).

   A fourteenth device type, `firmware/smoke-co-alarm/` (Matter Smoke/CO
   Alarm), followed door-lock — the user chose "Smoke/CO Alarm" from a
   short AskUserQuestion list (Smoke/CO Alarm / Occupancy Sensor / Air
   Quality Sensor / other). This repo's first device type over the
   SmokeCoAlarm cluster — a genuine life-safety alarm class, not a plain
   sensor readout or actuator — and, unlike firmware/door-lock/'s
   DoorLock cluster, one that IS "code-driven" in this SDK version
   (confirmed via a real `smoke_co_alarm/` folder under
   `data_model_provider/clusters/`), so its attributes go through
   `SmokeCoAlarmCluster`'s own setter API via the data model provider's
   registry, the same pattern `firmware/contact-sensor/`'s BooleanState
   and `firmware/light-sensor/`'s IlluminanceMeasurement already
   established. A real, previously-undocumented SDK gap was found and
   worked around by actually reading `SmokeCoAlarmCluster::
   HandleRemoteSelfTestRequest()`'s source rather than assuming: a real
   controller's SelfTestRequest command fully succeeds with no Delegate
   configured (the cluster sets `TestInProgress=true` on its own), but
   nothing ever clears that flag afterwards unless the app does — worth
   remembering as a bug class for any future `*Request`-style command
   cluster added to this repo (the SDK can set a flag on command receipt
   without ever owning the job of clearing it again).
   `SENSOR_TYPE` offers an MQ-2 (smoke) sensor, an MQ-7 (CO) sensor, or
   both together (the default, matching how real combination smoke+CO
   alarms are sold as one product) — deliberately a plain adjustable-
   millivolt-threshold classifier rather than a calibrated ppm reading,
   since MQ-series datasheets only document ppm as curves that shift per
   sensor/module/burn-in state, and Matter's own cluster has no numeric
   concentration attribute to report one into anyway (only the
   Normal/Warning/Critical `AlarmStateEnum`). The wizard integration
   needed one real design decision, resolved the same way as
   firmware/door-lock/'s Servo/Relay case: since the regular `driver`
   field always renders unconditionally, it was assigned to the MQ2 pin
   (this device's own shipped default), with the MQ7 pin riding on the
   Sensor picker's own `extraPickers` `pins` array instead — so the MQ2
   field stays visible-but-unused when "MQ-7 only" is selected, the same
   small, documented, harmless quirk as door-lock's own Servo/Relay
   tradeoff, not a new mechanism. Standard quick-power-cycle factory
   reset. Build-verified in Docker across all 3 sensor
   configs (MQ2+MQ7, MQ2-only, MQ7-only); not hardware-tested (no MQ-2/
   MQ-7 module physically available when written).

   A fifteenth device type, `firmware/occupancy-sensor/` (PIR motion,
   OccupancySensing cluster), followed directly after the Apple Home
   hardware-testing/debugging session documented below — deliberately
   built using esp-matter's own complete `endpoint::occupancy_sensor::
   create()` top-level helper rather than hand-assembling clusters, to
   confirm that path really does sidestep the missing-Descriptor-cluster
   bug class that session just found and fixed on color-light/
   addressable-light (it does — see that device type's own repository-
   layout entry above for the full technical detail). Build-verified in
   Docker and validated end to end on real hardware the same session:
   commissioned via Apple Home with zero errors, then real PIR motion
   correctly flipped the Home app's tile live, confirmed against a live
   serial log throughout.

   A sixteenth device type, `firmware/fan/` (FanControl, PercentSetting/
   PercentCurrent only), followed directly after occupancy-sensor —
   continuing the same "use the complete top-level helper" precedent
   (`endpoint::fan::create()`), and this repo's second genuine Delegate-
   based cluster after firmware/window-covering/'s WindowCovering.
   Landing the right integration pattern took two real, sequential Docker
   build failures (a compile error, then — even after that was fixed — a
   link error), root-caused by actually reading esp-matter's own
   `data_model_provider/clusters/fan_control/integration.cpp` rather than
   trusting connectedhomeip's generic `CodegenIntegration.h` declarations
   at face value: that header declares `Attributes::PercentCurrent::Set()`
   as if it were freely callable, but esp-matter's build substitutes its
   own integration.cpp for connectedhomeip's generic one, and esp-matter's
   version only implements `SetDefaultDelegate()` — not that free
   function — so the symbol was genuinely absent from the link. Fixed
   with the same registry-lookup-and-cast pattern firmware/
   contact-sensor/'s and firmware/occupancy-sensor/'s own setters already
   use. See its own repository-layout entry above for the complete
   detail, including why this is now a fourth genuinely distinct pattern
   for writing "code-driven" cluster attributes from app code in this
   repo. Build-verified in Docker; not hardware-tested (no PWM fan/MOSFET
   driver board physically available when written).

   A seventeenth device type, `firmware/air-quality-sensor/` (AirQuality +
   CarbonDioxideConcentrationMeasurement +
   TotalVolatileOrganicCompoundsConcentrationMeasurement, all on one
   endpoint), followed directly after fan — the user's choice from a
   short AskUserQuestion list (Air Quality Sensor / Water Leak Detector /
   Air Purifier / other). First device type combining a code-driven
   cluster's qualitative headline state with plain-ember-attribute
   numeric readings on the very same endpoint; confirmed as spec-
   legitimate directly against the CSA's own AirQualitySensor.xml before
   writing any code. A real, previously undocumented esp-matter gap was
   found and deliberately scoped around rather than patched: `air_quality
   ::create()` hardcodes FeatureMap to 0 and `air_quality::config_t` has
   no `feature_flags` field at all — unlike occupancy_sensing/
   smoke_co_alarm/fan_control/concentration_measurement, all of which
   properly thread a config-supplied feature bitmap through — so only
   AirQuality's base 3-state Good/Poor/Unknown scale is reachable through
   this helper today; the finer Fair/Moderate/VeryPoor/ExtremelyPoor
   states were left as a documented future step rather than risking an
   unverified FeatureMap override race against `AirQualityCluster`'s own
   constructor-time feature snapshot. CCS811's protocol (I2C address,
   nWAKE-to-GND wiring, boot sequence, register map, output ranges) was
   verified directly against ams's own datasheet and Programming Guide,
   fetched as PDFs and read via `pdftotext` — this repo's established
   practice for primary-source hardware protocol detail — catching the
   exact DRIVE_MODE bit position and both eCO2/eTVOC output ranges
   in-text rather than assumed from a community library. See its own
   repository-layout entry above for the complete detail. Build-verified
   in Docker; not hardware-tested (no CCS811 module physically available
   when written).

   An eighteenth device type, `firmware/water-leak-detector/` (BooleanState,
   device type 0x0043), followed directly after air-quality-sensor — the
   user's choice from a short AskUserQuestion list (Water Leak Detector /
   Air Purifier / Valve / other). The closest sibling to firmware/
   contact-sensor/ in this repo: esp-matter's own `water_leak_detector::
   add()` and `contact_sensor::add()` are structurally identical (Identify +
   BooleanState + StateChange event, both via `common::create<T>()`) —
   confirmed by reading both side by side rather than assumed from the
   device type's name alone. The one thing that genuinely differs, and
   the one thing most worth getting right, is StateValue's own direction:
   true means "leak detected" here, the OPPOSITE of contact-sensor's
   true=closed — confirmed against Espressif's own `MatterWaterLeakDetector`
   Arduino-ESP32 API and Apple's own HomeKit Leak Sensor characteristic
   direction before writing any code, not assumed from contact-sensor's
   own (opposite) convention. Also surfaced the same class of esp-matter
   FeatureMap gap air-quality-sensor found in AirQuality — here in
   `boolean_state::create()`, which never sets the ChangeEvent feature bit
   its own spec (WaterLeakDetector revision 2) requires — but unlike
   AirQuality's gap, this one was judged safe to fix directly (a plain
   `attribute::update()` on FeatureMap before `esp_matter::start()`)
   rather than just documented, since the underlying StateChange event
   fires unconditionally either way — confirmed by reading `boolean_state::
   event::create_state_change()` directly, this FeatureMap bit is pure
   advertised-conformance metadata here, not something gating real
   behavior the way AirQuality's Feature bits do. See its own repository-
   layout entry above for the complete detail. Build-verified in Docker;
   not hardware-tested (no water sensor module physically available when
   written).

   A nineteenth device type, `firmware/air-purifier/` (FanControl +
   HepaFilterMonitoring + ActivatedCarbonFilterMonitoring), followed
   directly after water-leak-detector — the user's choice from a short
   AskUserQuestion list (Air Purifier / Valve / Pressure Sensor / other).
   A direct extension of firmware/fan/: same FanControl Delegate, PWM
   output, and scope reused near-verbatim, plus the two filter-monitoring
   clusters that actually distinguish this device type from a plain Fan.
   Confirmed the same FeatureMap-hardcoded-to-0 gap air-quality-sensor and
   water-leak-detector both found also affects `resource_monitoring::
   create()` — but this time esp-matter DOES expose a real, public,
   documented way to enable the Condition feature afterwards
   (`cluster::resource_monitoring::feature::condition::add()`, a proper
   read-modify-write via `update_feature_map()`, not a raw attribute
   override), and a real, ready-made `GetClusterInstance()` free function
   for updating Condition/ChangeIndication at runtime — a fifth genuinely
   distinct pattern for writing code-driven cluster attributes from app
   code in this repo now. Filter life is a plain, adjustable time-based
   estimate (accumulated fan-running seconds against each filter's own
   configurable rated life in hours), persisted across reboots in its own
   NVS namespace, not a real sensor reading — same "smallest reasonable
   next step" reasoning firmware/window-covering/'s own time-based
   position estimate already applies. Two real, sequential compile errors
   were caught by an actual Docker build, not guessed: a wrong `feature`
   namespace nesting order, and a missing header for esp-matter's own
   `ResourceMonitoring::GetClusterInstance()` free function (declared in
   `data_model_provider/clusters/resource_monitor/integration.h`, not a
   connectedhomeip public header). See its own repository-layout entry
   above for the complete detail. Build-verified in Docker; not
   hardware-tested (no PWM fan/MOSFET driver board physically available
   when written).

   A twentieth device type, `firmware/valve/` (ValveConfigurationAndControl,
   device type 0x0042 "Water Valve"), followed directly after air-purifier
   — the user's choice from a short AskUserQuestion list (Valve /
   Pressure Sensor / Robot Vacuum / other). This repo's third genuine
   Delegate-based cluster, but the SDK owns noticeably more of the work
   here than FanControl or WindowCovering: Open/Close command handling,
   OpenDuration/RemainingDuration/AutoCloseTime bookkeeping, and the
   actual 1-second auto-close countdown timer are ALL handled inside
   `ValveConfigurationAndControlCluster` itself — confirmed by reading its
   .cpp directly, not assumed from the header — so this file implements
   none of its own timing logic, just relay actuation and reporting
   CurrentState back optimistically (same "app should trigger the state
   change" responsibility firmware/door-lock/'s own LockState already
   established). A second real, previously-undiscovered gap: unlike
   FanControl/ResourceMonitoring, esp-matter ships no public header
   declaring a `SetDefaultDelegate()`-style free function for this
   cluster at all — worked around with the registry-lookup-and-cast
   pattern instead, going straight through the cluster's own public
   `SetDelegate()` method. Researching this device type's own delegate
   registration is also what surfaced the `SetDefaultDelegate()`-called-
   before-`esp_matter::start()` bug in firmware/fan/ and firmware/
   air-purifier/ (see item 6 below) — this file's own registration was
   written correctly from the start once that timing model was
   understood. See its own repository-layout entry above for the
   complete detail. Build-verified in Docker; not hardware-tested (no
   relay/solenoid-valve hardware physically available when written).

   A twenty-first device type, `firmware/pressure-sensor/`
   (PressureMeasurement, device type 0x0305), followed directly after
   valve — the user's choice from a short AskUserQuestion list (Pressure
   Sensor / Robot Vacuum / Extractor Hood / other). The simplest device
   type XML in this repo so far (just Identify + one mandatory cluster).
   MeasuredValue's own unit (kPa, resolution 0.1 kPa) isn't spelled out
   in Matter's machine-readable cluster XML — confirmed instead against
   Home Assistant's own real, open-source Matter integration rather than
   assumed. BMP280's protocol (register map, compensation formula)
   verified against Bosch's own official datasheet, fetched as a PDF and
   read via `pdftotext`. A real, self-caught mistake during that process:
   the compensation formula was first written from memory (correctly, as
   it turned out) but its header comment cited the wrong datasheet
   section (the 32-bit fallback appendix instead of the actual 64-bit
   primary formula the code matches) — caught by re-checking the code
   against the literal fetched PDF text before finalizing, not assumed
   correct just because the code looked familiar. Worth remembering as a
   general discipline for any future compensation-formula code in this
   repo: always re-verify against the actual fetched source one more
   time before finalizing, even when it looks obviously right. See its
   own repository-layout entry above for the complete detail.
   Build-verified in Docker (clean first attempt); not hardware-tested
   (no BMP280 module physically available when written).

   A twenty-second device type, `firmware/robot-vacuum/` (RvcRunMode +
   RvcCleanMode + RvcOperationalState), followed directly after pressure-
   sensor — the user's choice from the same short AskUserQuestion list
   (Robot Vacuum was offered alongside Pressure Sensor/Extractor Hood).
   This repo's biggest cluster-integration surface in one device type so
   far: three separate command-handling clusters on one endpoint, two
   genuinely different SDK integration mechanisms for them (RvcRunMode/
   RvcCleanMode wired through esp-matter's own `config->delegate` field,
   auto-constructed internally by `esp_matter::start()`; RvcOperationalState
   needing a raw connectedhomeip `Instance`+`Delegate` pair built and
   `Init()`'d entirely by hand, since esp-matter's own `config_t` for that
   cluster is a literally empty struct with no delegate hook at all — a
   level deeper than firmware/valve/'s own ValveConfigurationAndControl
   gap), and this repo's first genuinely mobile actuator (two independent
   drive motors) rather than a single relay/PWM output. Both Mode
   clusters' integration shape, and RvcOperationalState's Instance/Delegate
   pair, were ported from connectedhomeip's own real, working reference
   code (`examples/chef/common/chef-rvc-mode-delegate.cpp` and
   `examples/rvc-app/rvc-common/`, both read directly) rather than guessed
   — same "port a real reference rather than guess the integration shape"
   precedent already used in this repo for SM2335EGH/APA102/OpenTherm.
   Explicitly, deliberately scoped out: any real navigation, obstacle
   avoidance, or return-to-dock path-finding (this firmware only ever
   drives both wheels forward at a fixed speed — there is no camera/
   LIDAR/bump/cliff sensor assumed) and the optional Service Area cluster
   (per-room/per-zone cleaning needs real room/map data this simple
   GPIO-level firmware has no way to generate) — both are the same
   category of honest, documented scope cut firmware/window-covering/'s
   own position-drift limitation already established, just larger here.
   See its own repository-layout entry above for the complete detail,
   including the real namespace-ambiguity compile error an actual Docker
   build caught and fixed. Build-verified in Docker; not hardware-tested
   (no robot chassis/motor-driver hardware physically available when
   written).

   Wizard integration for `firmware/robot-vacuum/` followed as a
   dedicated pass right after — initially deferred at the time it
   shipped (6 required + 1 optional GPIO field was more than the
   wizard's existing field shapes had needed to cover at once for any
   device type so far except firmware/camera/, which is excluded for a
   different, structural reason), then completed once its own effort
   budget: `driver` + a fixed 5-entry `extraButtons` array covers the 4
   drive-motor pins + vacuum + mop with zero new mechanism (the exact
   same "fixed set, not a variable count" mode firmware/color-light/'s
   Green/Blue channels already established), and the optional dock-
   contact sensor became a new `dockSensor` field — this repo's third
   parallel copy of the `statusLed`/`positionSensor` single-GPIO
   checkbox-gated shape, touching every site `positionSensor` itself
   needed when door-lock added it (enable-check helper, render block,
   validation, sed command, three summary-row renderers, device-type-
   switch reset, DOM event listeners) rather than reusing either literal
   field, since both carry hardcoded, device-type-specific copy that
   would misdescribe a charging-dock contact. Verified with the same
   Node.js sandboxed regression-check pattern this wizard's own history
   already establishes (device-type lookup; `renderConfigureDevice`
   output containing every field's own label/checkbox; `isProductComplete`
   both before any prior render — the same regression class
   `hasVariableButtonCount`'s and `positionSensor`'s own fixes were each
   checked against — and after, with the dock sensor off and on; the
   exact generated sed commands for all 7 `#define`s), then confirmed for
   real: those exact sed commands were run against a copy of the actual
   `firmware/robot-vacuum/main/app_main.cpp` and diffed against the
   original, a byte-for-byte match except the one line deliberately
   changed. No headless-Chromium screenshot pass this time (none was
   available in this environment) — judged acceptable since this change
   only reuses two mechanisms (`extraButtons`, `statusLed`/
   `positionSensor`) already screenshot-verified when they were first
   built, rather than introducing a new visual shape of its own. See
   `tools/product-wizard/README.md`'s own updated device-type list and
   its new paragraph on this addition for the user-facing detail.

   A twenty-third device type, `firmware/extractor-hood/` (Matter
   Extractor Hood), followed — the user's choice from a short
   AskUserQuestion list (this device type had already come up twice
   before as an unchosen option, alongside firmware/pressure-sensor/'s
   and firmware/robot-vacuum/'s own AskUserQuestion lists). The closest
   sibling to firmware/air-purifier/ in this repo: same FanControl
   Delegate, same LEDC PWM output, same HepaFilterMonitoring/
   ActivatedCarbonFilterMonitoring integration via `feature::
   condition::add()` + `ResourceMonitoring::GetClusterInstance()`, all
   reused near-verbatim. Confirmed directly against the CSA's own
   ExtractorHood.xml that FanControl is this device type's ONLY
   mandatory cluster — Identify itself is merely optional here (the
   first device type in this repo where that's true), so
   `endpoint::extractor_hood::create()` doesn't wire it in automatically;
   added onto the endpoint afterward anyway, same as every other device
   type here ships one. "HEPA Filter Monitoring" is knowingly repurposed
   to represent the hood's actual grease filter, since Matter has no
   dedicated grease-filter cluster — confirmed by reading the device
   type XML directly, which offers no alternative. See its own
   repository-layout entry above for the complete detail. Build-verified
   in Docker (clean first attempt); not hardware-tested (no PWM fan/
   MOSFET driver board physically available when written).

   Integrated into `tools/product-wizard/` immediately afterward — as
   expected, this one needed zero new wizard mechanism (unlike firmware/
   robot-vacuum/'s own new `dockSensor` field): a single `driver` (fan
   PWM) GPIO plus `identify`, the exact same shape firmware/fan/'s and
   firmware/air-purifier/'s own entries already use, plus a new hand-
   drawn icon (a canopy hood silhouette with steam/smoke rising into it
   — the one visual idea unique to this device type, distinct from fan/
   air-purifier's own circular blade motifs). Verified the same way as
   every wizard change this session: device-type lookup,
   `renderConfigureDevice` output containing both field labels,
   `isProductComplete` before and after render, and the exact generated
   sed commands for `HOOD_FAN_PWM_GPIO`/`IDENTIFY_LED_GPIO` — then those
   exact commands run for real against a copy of the actual `app_main.cpp`
   and diffed against the original, a byte-for-byte match (both GPIOs
   already shipped at their wizard defaults, confirming a correct no-op
   rather than a rewrite). See `tools/product-wizard/README.md`'s own
   updated device-type list and its new paragraph on this addition.

   A twenty-fourth device type, `firmware/water-heater/` (Thermostat[Heat]
   + WaterHeaterManagement + WaterHeaterMode), followed — the user's
   choice from a short AskUserQuestion list (Water Heater / EVSE / Generic
   Switch / Room Air Conditioner). The first device type in this repo to
   combine three previously-separate integration patterns on one endpoint
   at once (plain-ember-attribute Thermostat, ModeBase-derived Mode
   cluster, Delegate-driven command cluster with its own events), plus a
   genuinely new cluster (WaterHeaterManagement) with real Boost/
   CancelBoost command handling and BoostStarted/BoostEnded events. A
   real, previously-undocumented gap was found in connectedhomeip's own
   reference example (`examples/water-heater-app/`) — a literal
   "TODO: Implement Thermostat Cluster temperature handling. It's
   mandatory to be spec conformant." comment, confirming even the SDK
   authors haven't settled on one prescribed relationship between
   Thermostat's SystemMode and WaterHeaterMode's own Off/Manual/Timed;
   this file makes its own deliberate, documented choice (heating enabled
   only when both clusters agree, i.e. neither says Off) rather than
   guessing at an intended-but-unwritten convention. Two real, sequential
   compile errors were caught and fixed by an actual Docker build: the
   same `_span` literal-operator scoping issue firmware/robot-vacuum/
   already hit, and two wrong namespace-qualified type guesses
   (`WaterHeaterHeatSourceBitmap`, `Energy_mWh`) corrected against the
   compiler's own error output rather than assumed correct on the first
   attempt. See its own repository-layout entry above for the complete
   detail. Build-verified in Docker; not hardware-tested (no relay/
   DS18B20 probe hardware physically available when written).

   Integrated into `tools/product-wizard/` immediately afterward — needed
   one small reuse decision rather than any new mechanism: `driver` (the
   relay) plus `secondary` (the DS18B20 probe) reuses the exact two-
   independent-GPIO shape firmware/pressure-sensor/'s own I2C entry
   already established, confirmed safe to repurpose by reading the render
   code directly (`secondary`'s own copy is fully data-driven, no I2C-
   specific text hardcoded into the mechanism itself). Plus a new hand-
   drawn icon (a tank silhouette with a filled flame glyph near its base).
   Verified the same way as every wizard change this session: device-type
   lookup, `renderConfigureDevice` output containing all three field
   labels, `isProductComplete` before and after render, and the exact
   generated sed commands for `WATER_HEATER_RELAY_GPIO`/
   `WATER_HEATER_SENSOR_GPIO`/`IDENTIFY_LED_GPIO` — then those exact
   commands run for real against a copy of the actual `app_main.cpp` and
   diffed against the original, a byte-for-byte match (all three GPIOs
   already shipped at their wizard defaults, confirming a correct no-op).
   See `tools/product-wizard/README.md`'s own updated device-type list
   and its new paragraph on this addition.

   A twenty-fifth device type, `firmware/evse/` (EnergyEvse + EnergyEvseMode
   + DeviceEnergyManagement), followed — the user's choice from a short
   AskUserQuestion list (EVSE / Generic Switch / Refrigerator / Dishwasher
   or Laundry Washer/Dryer). This repo's first over the Energy EVSE cluster
   family, and its biggest single Delegate interface so far (~20 pure
   virtual methods, confirmed by reading `Delegate.h` directly). A real,
   checked discrepancy was found between the CSA's own EVSE.xml (which
   doesn't list DeviceEnergyManagement at all) and esp-matter's own
   top-level helper (which adds it unconditionally) — kept as shipped,
   left at its harmless default. `ChargingPreferences` (SetTargets/
   GetTargets/ClearTargets) is genuinely `<mandatoryConform/>`, so it's
   real, working, bounded in-memory storage — not a stub — but honestly
   never acted on by any scheduler, the same documented-limitation
   precedent WaterHeaterMode's own unused "Timed" mode already
   established. This device type ships with a prominent safety note at
   the top of `app_main.cpp` (and repeated in its own repository-layout
   entry above): no real SAE J1772/IEC 61851 Control Pilot protocol is
   implemented — the single relay is designed to gate an ALREADY-APPROVED
   EVSE unit's own low-voltage enable input, the same "gate an existing
   appliance's own control input" framing firmware/thermostat/'s RELAY
   output already uses for a boiler, never to switch AC mains or vehicle
   charging current directly. One real compile error was caught and fixed
   by an actual Docker build: `EnergyEvseCluster` needed its full
   `EnergyEvse::EnergyEvseCluster` qualification, the same "a namespace
   brought into scope doesn't bring its members in unqualified" lesson
   firmware/robot-vacuum/'s and firmware/water-heater/'s own `_span`/
   `WaterHeaterHeatSourceBitmap` fixes already taught — caught and fixed
   on the second Docker build attempt (clean after that). See its own
   repository-layout entry above for the complete detail. Build-verified
   in Docker; not hardware-tested (no EVSE hardware with a real external-
   enable input physically available when written).

   Integrated into `tools/product-wizard/` immediately afterward: `driver`
   (the relay) needed no new mechanism (same relay-as-driver shape
   firmware/valve/'s and firmware/water-heater/'s own entries already
   use), and the optional plug-detect input became a new `plugSensor`
   field — this repo's fourth parallel copy of the `statusLed`/
   `positionSensor`/`dockSensor` single-GPIO checkbox-gated shape, since
   each of those three carries its own hardcoded, device-type-specific
   copy that would misdescribe an EV "vehicle connected" signal. Plus a
   new hand-drawn icon (a rounded-square badge with a filled lightning
   bolt). Verified the same two-step way as every wizard change this
   session: a Node.js sandboxed check, then those exact generated sed
   commands run for real against a copy of the actual `app_main.cpp` and
   diffed against the original — the relay and identify lines a byte-for-
   byte no-op match, and the plug-sensor line separately confirmed to
   correctly rewrite `GPIO_NUM_NC` to a real pin once enabled. See
   `tools/product-wizard/README.md`'s own updated device-type list and
   its new paragraph on this addition.

   A twenty-sixth device type, `firmware/generic-switch/` (Switch
   cluster, momentary), followed — the user's choice from a short
   AskUserQuestion list (Generic Switch had already come up as a
   recommended-but-unchosen option in three previous rounds). This
   repo's first "smart button" accessory: fires real InitialPress/
   LongPress/ShortRelease/LongRelease/MultiPressOngoing/MultiPressComplete
   events for automations rather than sending a command to a bound
   target the way firmware/switch/'s own On/Off Switch does. Architecturally
   the simplest device type in several sessions (just Identify + Switch,
   both wired in automatically by the top-level helper — no manual
   Identify cluster call needed, the first time in a few device types),
   but the press-timing state machine itself was real, from-scratch
   engineering: checked directly that no SDK example (chef's own
   `SwitchManager.cpp` included) implements a real GPIO-to-press-timing
   driver, only test-event-injection harnesses. Built using the same
   industry-standard debounce/long-press/multi-press-window technique
   virtually every DIY multi-click button library uses, grounded in the
   cluster XML's own field constraints (confirmed that `MultiPressOngoing`
   never fires for a lone click but `MultiPressComplete` correctly does,
   reporting count=1 — a real, spec-checked detail, not assumed). Two
   real, sequential compile errors were caught and fixed by an actual
   Docker build: a missing `#include` for `SwitchCluster` itself, and a
   namespace gotcha worth remembering for any future `feature::xxx::add()`
   call in this repo — `feature` is nested inside EACH cluster's own
   namespace (`cluster::switch_cluster::feature::...`), not a flat
   `cluster::feature::` shared across every cluster. See its own
   repository-layout entry above for the complete detail. Build-verified
   in Docker; not hardware-tested (though this repo's other momentary-
   button device types already confirm the underlying breadboard-
   pushbutton wiring works on real hardware).

   Integrated into `tools/product-wizard/` immediately afterward — the
   simplest wizard entry in several sessions: a single `driver` (the
   button) plus `identify`, the exact same shape firmware/fan/'s own
   entry already uses (the press-timing state machine has no GPIOs of
   its own). Plus a new hand-drawn icon (a round button bezel with a
   filled cap and four radiating "click" ticks). Verified the same way
   as every wizard change this session: a Node.js sandboxed check, then
   those exact generated sed commands run for real against a copy of the
   actual `app_main.cpp` and diffed against the original — a byte-for-
   byte match. See `tools/product-wizard/README.md`'s own updated
   device-type list and its new paragraph on this addition.

   A twenty-seventh device type, `firmware/refrigerator/` (Refrigerator +
   Temperature Controlled Cabinet), followed — the user's choice from a
   short AskUserQuestion list (Refrigerator / Robot Vacuum-adjacent
   options had already been exhausted; Refrigerator was the recommended
   pick). This repo's first genuinely composed, multi-endpoint device: a
   Refrigerator root endpoint with two Temperature Controlled Cabinet
   *child* endpoints (Fridge + Freezer) linked via esp-matter's real
   `set_parent_endpoint()` API, confirmed against the CSA's own
   `<conditionRequirements>` block (a real spec-level structural
   requirement, not a design choice) and against esp-matter's own official
   `examples/refrigerator/` reference. Research surfaced and resolved a
   real open question before any code was written: whether
   `TemperatureControlCluster::SetDelegate()` (which the official
   reference always calls) is actually needed for TN-only mode — confirmed
   NO by reading `TemperatureControlCluster.cpp` directly, since
   `SetDelegate()`'s target is only ever read behind the TL feature flag,
   which this device never sets. Implementation then surfaced a genuinely
   new class of gap for this repo: esp-matter ships two parallel cluster/
   endpoint implementations ("legacy", the actual default this repo's own
   sdkconfig.defaults compiles against, and a newer "generated" one gated
   behind a Kconfig flag this repo has never enabled) that silently
   disagree with each other — a feature-flag auto-set behavior, a config
   field name (`temp_setpoint` vs. `temperature_setpoint`), and an entire
   C++ wrapper namespace (`refrigerator_and_tcc_mode` vs. `refrigerator_
   and_temperature_controlled_cabinet_mode`) all differ between the two,
   and code initially written against the "generated" behavior (since
   that's what a first read of the SDK source showed) failed to compile
   against the actual "legacy" default — caught and fixed by an actual
   Docker build, not guessed. Worth remembering for any future device
   type using a less-common cluster: esp-matter's SDK source should be
   read from the SAME implementation (legacy vs. generated) the project's
   own sdkconfig actually compiles against, not whichever one a search
   happens to surface first. See its own repository-layout entry above
   for the complete detail. Build-verified in Docker; not hardware-tested
   (no relay/DS18B20/reed-switch hardware for this device type physically
   available when written).

   A twenty-eighth device type, `firmware/dishwasher/` (OperationalState +
   TemperatureControl + DishwasherMode + DishwasherAlarm), followed — the
   user's choice from a short AskUserQuestion list (Dishwasher had already
   come up as a recommended-but-unchosen option alongside water-heater and
   evse). This repo's first over the *generic* OperationalState cluster
   (0x0060), used directly rather than derived the way RvcOperationalState
   is — and a genuinely new command-cycle shape (Start/Stop/Pause/Resume
   driving a real, if simplified, timed wash cycle) rather than a
   continuous regulation loop or a one-shot actuation. A real naming
   gotcha was found before any code was written: esp-matter's own
   generated files use `dish_washer` (with an underscore) throughout,
   not matching the CSA's own un-underscored "Dishwasher" naming at all —
   a first source search for "dishwasher" came back nearly empty as a
   result, only resolved by widening the search to "dish.washer".
   OperationalState itself turned out to support esp-matter's own
   automatic `config->delegate` wiring (unlike RvcOperationalState's empty
   `config_t`), but reaching the live cluster instance from OUTSIDE the
   delegate's own callbacks (needed for the door sensor's own async
   safety-pause, and for the wash cycle finishing on its own) needed
   esp-matter's own `get_delegate_managed_instance()` — a sixth genuinely
   distinct "how do I reach a live cluster instance from app code" pattern
   in this repo now (see its own repository-layout entry above for the
   full enumeration of all six). DishwasherAlarm turned out to have a
   genuinely complete Delegate + Server API (its own `SetStateValue()`
   fires the Notify event internally) — unlike firmware/refrigerator/'s
   own RefrigeratorAlarm, no manual event-rigging gap this time, though
   the same FeatureMap-hardcoded-to-0 gap class still applies to its Reset
   feature. Four real compile errors were caught and fixed across two
   failed Docker build attempts, not guessed — see its own repository-
   layout entry above for the complete detail on all four. Build-verified
   in Docker; not hardware-tested (no relay/DS18B20/reed-switch hardware
   for this device type physically available when written).

   A twenty-ninth device type, `firmware/laundry-washer/` (OperationalState
   + TemperatureControl + LaundryWasherMode + LaundryWasherControls),
   followed — the user's choice from a short AskUserQuestion list
   (Laundry Washer had already come up as a recommended-but-unchosen
   option alongside water-heater and evse, the same way Dishwasher had).
   The closest sibling to firmware/dishwasher/ in this repo — same
   OperationalState/TemperatureControl patterns, a fourth ModeBase-derived
   Mode cluster — plus one genuinely new cluster, LaundryWasherControls,
   confirmed to need BOTH a real FeatureMap (set correctly here — this
   cluster's own `create()` sets FeatureMap twice, once hardcoded to 0
   then immediately overwritten with `config->feature_flags`, so unlike
   RefrigeratorAlarm/AirQuality there's no real gap) AND a separate
   Delegate supplying its two supported-value lists (SpinSpeeds/
   SupportedRinses) — while its actual current-value attributes
   (SpinSpeedCurrent/NumberOfRinses) are plain ember attributes a
   controller writes directly, no command needed. NumberOfRinses is the
   one setting in this file given real physical meaning: tracked via
   `attribute::PRE_UPDATE`, it sets how many real Rinse-then-Drain phases
   the simulated wash cycle actually runs. Unlike firmware/dishwasher/'s
   own `dish_washer`/`dish_washer_mode` naming mismatch, esp-matter's own
   wrapper names here match the CSA's naming (confirmed directly rather
   than assumed to generalize from the Dishwasher case). Build-verified in
   Docker — clean on the first attempt, the direct payoff of applying
   firmware/dishwasher/'s own hard-won lessons (the `CodegenIntegration.h`
   include, the `get_delegate_managed_instance()` qualification, the
   legacy-vs-generated TemperatureControl pitfall) proactively rather than
   rediscovering them. See its own repository-layout entry above for the
   complete detail. Not hardware-tested (no relay/DS18B20/reed-switch
   hardware for this device type physically available when written).

   A thirtieth device type, `firmware/pump/` (PumpConfigurationAndControl
   + On/Off + LevelControl), followed laundry-washer — this repo's first
   over the PumpConfigurationAndControl cluster, and, after three
   sessions building OperationalState- or hysteresis-loop-driven device
   types in a row (Dishwasher/Laundry Washer/Refrigerator), its first
   plain continuous-control-loop-free one: a pump reacts directly to a
   controller's own On/Off, CurrentLevel, and OperationMode writes, with
   no background task of its own needed at all. Confirmed directly
   against the CSA's own Pump.xml that Identify/On-Off/
   PumpConfigurationAndControl are all genuinely mandatory together — a
   real, non-optional trio, unlike almost every other device type this
   repo has built, where Identify alone is consistently the one optional
   cluster. `endpoint::pump::create()`'s own `config_t` constructor
   turned out to already set Identify's type
   (`Identify::IdentifyTypeEnum::kActuator`) itself — the first top-level
   helper in this repo confirmed to do that internally rather than
   needing the call site to set it explicitly, caught by reading the
   constructor body directly rather than assumed from every prior device
   type's own pattern. PumpConfigurationAndControl enables only the
   ConstantSpeed (SPD) feature — the one of its five "choice, at least 1"
   control-mode features that maps directly onto a PWM-driven speed with
   no pressure/flow/temperature sensor hardware needed, same "smallest
   reasonable next step" scoping as every other device type's own
   first-cut feature choice — and, unlike RefrigeratorAlarm/AirQuality's
   own documented FeatureMap gap, this cluster's `create()` was confirmed
   to thread `config->feature_flags` through correctly with no gap to
   work around. OperationMode (Normal/Minimum/Maximum/Local) is a plain
   writable ember attribute — no Delegate at all on this cluster,
   confirmed by reading its legacy `config_t` directly — that genuinely
   drives the real PWM output: Normal follows LevelControl's own
   CurrentLevel, Minimum/Maximum instead run at the feature-configured
   MinConstSpeed/MaxConstSpeed bounds regardless of CurrentLevel
   (matching the spec's own wording for those two modes), and Local is
   accepted but honestly behaves identically to Normal (no separate
   physical control panel to defer to on this hobby build). The
   cluster's own rich seventeen-event fault-reporting set
   (SupplyVoltageLow/High, DryRunning, PumpBlocked,
   MotorTemperatureHigh, Leakage, AirDetection, etc.) is not fired
   anywhere — every one needs real fault-detection hardware this
   hobby-scale build doesn't have, the same "no sensor, no fabricated
   fault reporting" honesty precedent firmware/evse/'s always-NoError
   FaultState already establishes. See its own repository-layout entry
   above for the complete detail. Build-verified in Docker; not
   hardware-tested (no pump/relay/PWM-speed-controller hardware for this
   device type physically available when written).

   A thirty-first device type, `firmware/laundry-dryer/` (OperationalState
   + TemperatureControl + LaundryWasherMode (reused) +
   LaundryDryerControls), followed pump — the user's choice from a short
   AskUserQuestion list (recommended as the natural sibling to firmware/
   laundry-washer/). The closest sibling to firmware/laundry-washer/ in
   this repo, and this repo's first laundry appliance with no water
   handling at all (no fill, no rinse, no drain — just heat and tumble).
   A real, spec-level detail found before writing any code: the CSA's own
   LaundryDryer.xml reuses LaundryWasher's own Mode cluster (0x0051)
   verbatim — there is no separate "Laundry Dryer Mode" cluster at all,
   confirmed by reading the XML directly rather than assumed from the
   device type's own name. Two constraints layered onto that reused
   cluster specifically for a dryer (the DEPONOFF feature and the
   StartUpMode attribute, both disallowed) turned out to need no code at
   all — `laundry_washer_mode::create()` already hardcodes FeatureMap to 0
   and never creates a StartUpMode attribute, so both were already
   unreachable through the same helper firmware/laundry-washer/ already
   uses. A genuinely new, previously-undocumented gotcha was found by
   reading `laundry-dryer-controls-server.cpp` directly rather than
   assuming LaundryDryerControls behaves like LaundryWasherControls: its
   own `PreAttributeChangedCallback` calls `VerifyOrDie(delegate !=
   nullptr)` before validating a SelectedDrynessLevel write — a
   controller writing that attribute with no delegate registered would
   abort the whole device outright, not silently no-op, unlike every
   other optional-delegate cluster built in this repo so far; the
   delegate is therefore always set here, never left null. SelectedDryness
   Level is the one attribute given real physical meaning (Low/Normal/
   Extra/Max map to 20/35/50/65 minutes of drying), driving a 2-phase
   Drying-then-Cooldown cycle — the unheated Cooldown tail is standard
   real tumble-dryer behavior (reduces wrinkling and hot-lint fire risk),
   not invented for this file. See its own repository-layout entry above
   for the complete detail. Build-verified in Docker — clean on the first
   attempt, the direct payoff of researching firmware/laundry-washer/'s
   and firmware/dishwasher/'s own hard-won OperationalState/
   TemperatureControl lessons proactively before writing any code; not
   hardware-tested (no relay/DS18B20/reed-switch hardware for this device
   type physically available when written).

   A thirty-second device type, `firmware/room-air-conditioner/`
   (Thermostat[Cooling only] + On/Off[DeadFrontOnOff] + FanControl),
   followed laundry-dryer — the user's choice from a short AskUserQuestion
   list (Room Air Conditioner had already come up as a recommended-but-
   unchosen option twice before, during firmware/evse/'s and firmware/
   water-heater/'s own rounds). This repo's first device type to combine
   firmware/thermostat/'s own Thermostat control-loop pattern with
   firmware/fan/'s own FanControl Delegate pattern on a single endpoint.
   A real, previously-unseen spec detail was found before writing any
   code: unlike every appliance device type built so far (Dishwasher/
   Laundry Washer/Laundry Dryer), where On/Off's DeadFrontOnOff feature is
   optionalConform and simply left out, Room Air Conditioner makes BOTH
   that On/Off cluster AND a separate Thermostat SystemMode
   mandatoryConform at once — two genuinely different concepts of "on"
   coexist on the same endpoint for the first time in this repo. Resolved
   with a deliberate, documented choice: no GPIO is tied to the OnOff/
   DeadFrontOnOff attribute at all (left exactly as the top-level helper
   wires it — a cosmetic "is the unit's own display lit" flag with no
   physical effect in this hobby build); the actual compressor relay is
   driven entirely by Thermostat's own SystemMode/hysteresis loop instead,
   a Cool-only subset of firmware/thermostat/'s own control loop (no Heat
   branch at all, since ControlSequenceOfOperation is CoolingOnly).
   FanControl itself is optionalConform here (unlike firmware/fan/'s/
   firmware/air-purifier/'s/firmware/extractor-hood/'s own device types,
   where it's mandatory and auto-wired by their own top-level helpers) —
   added manually via the lower-level `cluster::fan_control::create()`
   free function instead, with its Delegate deliberately registered via
   connectedhomeip's own `FanControl::SetDefaultDelegate()` free function
   AFTER `esp_matter::start()` rather than through that same function's
   own `config->delegate` field, following firmware/air-purifier/'s and
   firmware/extractor-hood/'s own proven-correct convention (not the
   cluster-level config_t's own delegate field, which this repo's history
   with firmware/fan/'s own ordering bug already taught not to trust
   without re-confirming). See its own repository-layout entry above for
   the complete detail. Build-verified in Docker — clean on the first
   attempt; not hardware-tested (no relay/DS18B20/fan-driver hardware for
   this device type physically available when written).

   A thirty-third device type, `firmware/heat-pump/` (Heat Pump root +
   Thermostat[Heat+Cool] child endpoint), followed room-air-conditioner —
   the user's choice from a short AskUserQuestion list (Heat Pump / Flow
   Sensor / Humidity Sensor; Heat Pump had already come up as a
   recommended-but-unchosen option during firmware/room-air-conditioner/'s
   own round). This repo's second genuinely composed, multi-endpoint device
   after firmware/refrigerator/, but composed very differently — and
   against real, conflicting guidance from three separate sources
   (the current ratified CSA XML, esp-matter's own `heat_pump::add()`
   implementation, and connectedhomeip's own chef reference device) that
   genuinely disagreed with each other on how a heat pump's own temperature-
   control surface should be modeled, resolved by following the ratified
   XML's own stated intent (a composed Thermostat child endpoint) while
   keeping esp-matter's own proven same-endpoint composition for the
   Electrical Sensor part (confirmed correct by BOTH esp-matter's own
   implementation and the independent chef reference agreeing on that one
   specific point). A real, previously-undocumented composition style was
   found and confirmed: unlike firmware/refrigerator/'s own child-endpoint
   pattern, `electrical_sensor::add()` calls `add_device_type()` on
   whatever endpoint it's handed rather than creating a child — meaning a
   single endpoint's own DeviceTypeList can legitimately carry more than
   one device type when the spec calls for it, a genuinely different, valid
   composition style this repo hadn't used before. The Thermostat child
   endpoint reuses firmware/thermostat/'s own Heat+Cool hysteresis control
   loop closely (unlike firmware/room-air-conditioner/'s own deliberately
   Cool-only scope) — a heat pump's whole point is doing both — driving a
   compressor relay (runs in either mode) and a separate reversing-valve
   relay (tracks SystemMode directly, independent of the compressor's own
   on/off cycling, matching how a real reversing valve is pre-positioned
   before the compressor starts rather than toggled per hysteresis cycle).
   PowerSource/ElectricalSensor/DeviceEnergyManagement are all left
   structurally present but not driven by any real sensor (both
   ElectricalPowerMeasurement's and ElectricalEnergyMeasurement's own
   legacy `cluster::create()` functions confirmed to tolerate a null
   delegate with no crash risk) — same "no sensor, no fabricated data"
   honesty precedent firmware/evse/'s own always-NoError FaultState
   already establishes. See its own repository-layout entry above for the
   complete detail. Build-verified in Docker — clean on the first attempt
   despite the composition complexity; not hardware-tested (no relay/
   DS18B20 hardware for this device type physically available when
   written).

   A thirty-fourth device type, `firmware/flow-sensor/` (FlowMeasurement),
   followed heat-pump — the user's choice from a short AskUserQuestion list
   (Flow Sensor had already come up as a recommended-but-unchosen option
   twice before, during firmware/room-air-conditioner/'s and firmware/
   heat-pump/'s own rounds). Back to this repo's simplest recent shape
   after firmware/heat-pump/'s own composition complexity — the same
   minimal Identify+one-mandatory-cluster XML shape firmware/
   pressure-sensor/'s own device type already established, and confirmed
   FlowMeasurement is the same "code-driven" cluster category (registry-
   lookup `SetMeasuredValue()`) as PressureMeasurement/
   TemperatureMeasurement. MeasuredValue's own m3/h-times-10 encoding
   wasn't spelled out in Matter's own machine-readable cluster XML (same
   gap firmware/pressure-sensor/'s own header comment already documents)
   — confirmed instead against the same real source already used for that
   file, Home Assistant's own Matter integration source, rather than
   assumed. The sensor itself — a Hall-effect pulse-output flow sensor,
   YF-S201-class — reuses firmware/outlet/'s own GPIO-ISR pulse-counting
   technique (already established there for its BL0937/HLW8012/CSE7759
   power-monitor drivers) rather than introducing a new pattern; its own
   pulse characteristic (`F(Hz) = 7.5 * Q(L/min)`) was cross-checked across
   multiple independent sources, since — like several other widely cloned
   hobbyist modules already in this repo (the contact sensor's reed
   switch, the occupancy sensor's PIR module) — no single canonical
   datasheet exists for it. See its own repository-layout entry above for
   the complete detail. Build-verified in Docker (clean first attempt); not
   hardware-tested (no YF-S201-class sensor physically available when
   written).

   A thirty-fifth device type, `firmware/humidity-sensor/`
   (RelativeHumidityMeasurement), followed flow-sensor — the user's choice
   from a short AskUserQuestion list (Humidity Sensor had already come up
   as a recommended-but-unchosen option three times before, during
   firmware/room-air-conditioner/'s, firmware/heat-pump/'s, and firmware/
   flow-sensor/'s own rounds). A standalone sibling to firmware/
   temperature-sensor/'s own humidity endpoint — confirmed by reading the
   CSA's own HumiditySensor.xml directly that this device type has no
   optional TemperatureMeasurement slot at all, unlike RoomAirConditioner's
   own optional Temperature/Humidity pair. Reused firmware/
   temperature-sensor/'s exact 6 humidity-capable sensor drivers verbatim
   (SHT3x/SHT4x/AHT20/DHT11/DHT22/BME280) rather than writing anything new
   — the one deliberate, documented difference from that file's own
   7-sensor library: DS18B20 isn't offered at all here, since it measures
   temperature only and a humidity-only device type has nothing to read
   from it (unlike firmware/temperature-sensor/'s own `SENSOR_HAS_HUMIDITY`
   compile-time escape hatch, which still needs DS18B20 for its primary
   temperature endpoint). See its own repository-layout entry above for the
   complete detail. Build-verified in Docker for all 6 `SENSOR_TYPE`
   values (each one individually rebuilt and confirmed, not just the
   shipped default); not hardware-tested (no fresh hardware pass was done
   for this addition).

   A thirty-sixth device type, `firmware/water-freeze-detector/`
   (BooleanState), followed humidity-sensor — the user's choice from a
   short AskUserQuestion list (Water Freeze Detector / Soil Sensor /
   Dimmable Plug-In Unit). The closest sibling to firmware/
   water-leak-detector/ in this repo — same BooleanState cluster, same
   "true = alarm/problem" StateValue direction (confirmed against
   Espressif's own `MatterWaterFreezeDetector` Arduino-ESP32 class
   directly, mirroring the exact same verification already done for
   firmware/water-leak-detector/'s own StateValue direction), and the
   identical real esp-matter FeatureMap gap + fix (`boolean_state::
   create()` hardcodes FeatureMap to 0, never setting the ChangeEvent
   feature bit this device type's own spec makes mandatory — safe to
   overwrite directly since the StateChange event fires unconditionally
   either way). The one genuinely different design choice: unlike water
   leaks, there's no equally common, hobby-accessible "freeze switch"
   module to point to with the same confidence, so this file reuses the
   DS18B20 driver already established across this repo's other appliance/
   HVAC device types instead, paired with a plain adjustable threshold
   (3.00 degC, a few degrees above actual freezing to leave response
   time) + hysteresis classifier — same "adjustable threshold, not a
   calibrated reading" precedent firmware/smoke-co-alarm/'s and firmware/
   air-quality-sensor/'s own classifiers already establish. See its own
   repository-layout entry above for the complete detail. Build-verified
   in Docker (clean first attempt); not hardware-tested (no DS18B20
   hardware for this device type physically available when written).

   A thirty-seventh device type, `firmware/soil-sensor/` (Soil
   Measurement), followed water-freeze-detector — the user's choice from
   a short AskUserQuestion list (Soil Sensor had already come up as a
   recommended-but-unchosen option twice before). This repo's first over
   the Soil Measurement cluster, and one that surfaced a genuinely new,
   more severe class of esp-matter gap than anything found in this repo
   before: `soil_measurement::create()`'s own config_t is a literally
   empty struct, and reading the real init callback its `create()`
   registers (`data_model_provider/clusters/soil_measurement/
   integration.cpp`) directly — not assumed complete from the header
   alone — found a literal `VerifyOrDieWithMsg(gLimits.find(endpointId)
   != gLimits.end(), ...)`: skip the one required
   `SoilMeasurement::SetSoilMoistureLimits()` call before
   `esp_matter::start()` and the device doesn't misbehave quietly like
   every prior FeatureMap-class gap this repo has catalogued — it hard-
   crashes the whole firmware at startup. Worth remembering as a new
   category for any future code-driven cluster whose real construction is
   deferred to an init callback: read that callback's own source
   directly, since a missing call isn't always a silent gap the way it
   has been every other time so far. `SetSoilMoistureMeasuredValue()`
   itself, by contrast, is reached via esp-matter's own ready-made free
   function — the same "convenience free function" category firmware/
   air-purifier/'s own `ResourceMonitoring::GetClusterInstance()` already
   established, just for a cluster this repo hadn't used before. The
   sensor itself (a cheap capacitive soil-moisture probe, analog output)
   reuses firmware/light-sensor/'s own ADC1 + calibration-scheme driver
   pattern, but — unlike that sensor's real datasheet-grounded
   characteristic curve — needs a plain two-point field calibration
   instead (dry air = 0%, submerged in water = 100%), documented as
   adjustable placeholder defaults rather than a measured calibration.
   See its own repository-layout entry above for the complete detail.
   Build-verified in Docker (clean first attempt, despite the genuinely
   new cluster integration); not hardware-tested (no capacitive soil-
   moisture sensor physically available when written).

   A thirty-eighth device type, `firmware/dimmable-plug/`
   (DimmablePlug-InUnit — OnOff + LevelControl on a plug-in-unit endpoint),
   followed soil-sensor — the user's choice from a short AskUserQuestion
   list (Dimmable Plug-In Unit had already come up as a recommended-but-
   unchosen option twice before). The natural combination of two device
   types already in this repo — firmware/outlet/'s own plug-in framing and
   firmware/dimmable-light/'s own LevelControl/PWM output — reused
   directly rather than reinvented, right down to the exact LEDC settings
   and the OnLevel-null bug fix that firmware/dimmable-light/'s own
   hardware testing already confirmed correct. The one genuine judgment
   call: unlike firmware/outlet/'s own default RELAY output, a relay
   physically cannot dim anything, so this device type's output is real
   PWM (a DC load through a MOSFET, or a real commercial AC dimmer
   module's own PWM/analog dimming-control input) rather than a relay —
   explicitly NOT an attempt to switch mains current directly, the same
   "gate an existing device's own control input" framing firmware/
   thermostat/'s and firmware/evse/'s own relay outputs already establish
   for their own mains-adjacent hardware. See its own repository-layout
   entry above for the complete detail. Build-verified in Docker (clean
   first attempt); not hardware-tested (no MOSFET/dimmer-module hardware
   for this device type physically available when written).

   A thirty-ninth device type, `firmware/rain-sensor/` (BooleanState),
   followed dimmable-plug — the user's choice from a short AskUserQuestion
   list (Rain Sensor had already come up as a recommended-but-unchosen
   option before). The third sibling in this repo's BooleanState family
   after firmware/water-leak-detector/ and firmware/water-freeze-detector/
   — same cluster, same real esp-matter FeatureMap gap and fix (see those
   two files' own header comments, and now this one's, for the full
   detail). A real, worth-remembering finding: this device type's own
   sensor module is confirmed, via multiple independent sources rather
   than assumed from the name alone, to be literally the SAME physical
   board as firmware/water-leak-detector/'s own probe — a widely cloned
   LM393-comparator design sold interchangeably as either a "water
   sensor" or "rain sensor" depending on mounting — so this file reuses
   that earlier file's own GPIO-ISR debounce logic verbatim rather than
   writing anything new. StateValue direction ("true = rain detected")
   was confirmed against Espressif's own `MatterRainSensor` Arduino-ESP32
   class directly, the same verification rigor already applied to the
   other two BooleanState siblings' own StateValue directions. See its
   own repository-layout entry above for the complete detail.
   Build-verified in Docker (clean first attempt); not hardware-tested
   (no rain sensor module physically available when written).

   A fortieth device type, `firmware/color-temperature-light/`
   (ColorTemperatureLight — OnOff + LevelControl + ColorControl[CT only]),
   was scaffolded (root/main CMakeLists.txt, partitions.csv,
   sdkconfig.defaults) but interrupted by a power outage before
   `main/app_main.cpp` existed; resumed and completed in the next session.
   This repo's first tunable-white bulb: cool-white + warm-white channels
   only, no RGB at all. Confirmed directly against the CSA's own
   ColorTemperatureLight.xml (0x010C) that, unlike ExtendedColorLight
   (0x010D, what firmware/color-light/ and firmware/addressable-light/
   both implement), only the ColorTemperature feature is mandatory for
   ColorControl — HueSaturation/XY aren't part of this device type's
   requirements at all, so `endpoint::color_temperature_light::create()`
   is a genuinely simpler, more correct fit for hardware that only does
   cool/warm blending. Confirmed by reading esp-matter's own legacy
   `color_temperature_light::add()` directly that — unlike color-light's/
   addressable-light's own hand-assembled ExtendedColorLight endpoints —
   this is a COMPLETE top-level helper (correct mandatory clusters/
   features/attributes, auto-created Descriptor cluster via the shared
   `common::create<T>()` template), sidestepping both real bugs those two
   files needed fixed after real Apple Home hardware testing. No
   color-space interlock needed either, unlike color-light's RGBWW mode —
   this device type has exactly one color feature, so there's nothing to
   interlock between. `mireds_to_cw_ww()`, the OnLevel-null fix, and the
   6500K/2700K cool/warm default Kelvin values are all reused verbatim
   from firmware/color-light/'s own RGBWW mode. See its own repository-
   layout entry above for the complete detail. Build-verified in Docker
   (one real, quickly-caught compile error — `fminf`/`fmaxf` need
   `<cmath>` — fixed); not hardware-tested (no cool-white/warm-white LED/
   driver board for this device type physically available when written).

   A forty-first device type, `firmware/closure/` (Closure Control —
   garage door / roller shutter / awning), followed — the user's choice
   from a short AskUserQuestion list (Closure / Microwave Oven / Doorbell).
   This repo's first over the Closure Control cluster, a brand-new
   (Matter 1.6) cluster family. Confirmed directly against the CSA's own
   Closure.xml (0x0230) that Window Covering and Closure Dimension are
   both explicitly disallowed on this device type (they belong to a
   separate ClosurePanel child-endpoint device type instead, for
   multi-panel closures — not implemented here). `endpoint::closure::
   create()` confirmed complete/ready-to-use. Positioning-only scope, same
   "smallest reasonable next step" precedent as firmware/window-covering/'s
   own Lift-only choice — maps directly onto a simple two-relay motor with
   no position sensor. The real find here: a genuinely new, EIGHTH
   "how do I wire up a code-driven cluster's real implementation from app
   code" pattern for this repo, and the first one requiring the OPPOSITE
   ordering from every prior Delegate-based cluster — ClosureControl's own
   `Config` constructor takes its delegate as a mandatory reference, so
   esp-matter's own cluster-construction init callback (which runs as part
   of `esp_matter::start()`) fails outright unless the delegate was already
   registered via `MatterClosureControlSetDelegate()` BEFORE that call —
   confirmed by reading esp-matter's own `data_model_provider/clusters/
   closure_control/integration.cpp` and connectedhomeip's own
   `ClosureControlCluster::Config` constructor directly, not assumed. This
   finding, like firmware/fan/'s own `SetDefaultDelegate()`-ordering bug
   before it, is sourced from reading source directly and hasn't itself
   been hardware-confirmed yet. See its own repository-layout entry above
   for the complete detail, including what the cluster's own `HandleMoveTo()`/
   `SetMainState()` already do automatically vs. what this file's own
   `closure_task` has to do (live position/SecureState reporting, deciding
   when travel completes, firing MovementCompleted/SecureStateChanged).
   Build-verified in Docker (clean first attempt, despite the ordering
   subtlety above); not hardware-tested (no garage-door/roller-shutter
   motor+relay hardware for this device type physically available when
   written).
2. Implement Matter **OTA** — partially done. All forty-one firmware
   types ship `CONFIG_ENABLE_OTA_REQUESTOR=y`, which adds the OTA Requestor
   cluster to the root node endpoint entirely via Kconfig — esp-matter's
   own core startup (`esp_matter_core.cpp`) calls
   `esp_matter_ota_requestor_init()`/`_start()` automatically once that
   flag is on, so no app code was needed. Confirmed on real hardware for
   `firmware/contact-sensor/` and `firmware/switch/` (clean boot, cluster
   registered, zero errors); the other thirty-nine build identically since
   the code path is generic to every device type, not device-specific.

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
4. Two cross-cutting features added to **all ten** device types at once
   (not staged one at a time), both requested together after a set of
   screenshots from a real manufacturing/config tool showing a much
   richer "Indicators" state machine and a "Factory Reset" tab than
   anything this repo had:

   **Optional RGB status LED (later removed, see below).** Built,
   documented, and extended to every device type added afterward —
   color + blink/breathe patterns showing real commissioning/Identify
   state, sourced from connectedhomeip's own `CHIPDeviceEvent.h`
   lifecycle events and the Identify cluster's own
   `EffectIdentifierEnum` — but explicitly not what the user actually
   wanted on real hardware, and removed entirely (firmware code, the
   wizard's `rgbStatusLed` mechanism, and its documentation) once that
   became clear. Worth remembering if this ever comes up again: the
   feature itself worked and was hardware-agnostic to add, the call was
   about product fit, not a technical problem with the implementation.

   **Quick-power-cycle factory reset.** Power the device off and on 3
   times in a row (roughly a couple of seconds each way) and it
   factory-resets and re-enters commissioning setup mode — no button or
   extra pin needed, matching the screenshot's own "Power off... wait 2s
   ... power on... wait 2s... repeat 3 times" instructions, and the same
   mechanism real plug-in/hardwired smart-home devices commonly use since
   they often have no accessible reset button once installed (Tasmota's
   own "Quick Power Cycle" detection works the same way). A plain counter
   in its own `"boot_info"` NVS namespace (deliberately separate from
   esp_matter's/Matter's own storage) increments on every boot and starts
   a one-shot 10-second timer; if the device stays powered that long
   without another reboot, the counter clears back to 0 (a "confirmed"
   normal boot — so an ordinary power outage or a single unplug/replug
   never accidentally triggers it). 3 reboots landing before that timer
   fires calls `esp_matter::factory_reset()` (declared in
   `esp_matter_core.h`: "Perform factory reset and erase the data stored
   in the non volatile storage. This also restarts the device."). Its
   call site matters: `factory_reset()`'s own implementation (read
   directly in `esp_matter_core.cpp`) calls
   `chip::Server::GetInstance().ScheduleFactoryReset()`, which needs the
   Matter server already running — confirmed by cross-checking
   esp-matter's own reference `app_reset` component
   (`examples/common/app_reset/app_reset.cpp`), which only ever calls it
   from a runtime button callback, never during boot. So the boot-count
   check runs early (right after `nvs_flash_init()`) and only *decides*
   whether a reset is due; the actual `factory_reset()` call happens
   later in `app_main()`, after `esp_matter::start()` has completed.

   Quick-power-cycle factory reset is Docker build-verified across all
   forty-one device types — not yet hardware-tested. The wizard
   (`tools/product-wizard/`) has a static "Factory reset" info box
   rendered directly under the Configuration summary sidebar on every
   device type (wrapped together in a `.config-sidebar-stack` flex
   column so it stays in the same grid column regardless of whether that
   step also has a left-hand picker sidebar). It needed no wizard
   mechanism beyond that box — it has no configurable GPIOs or
   `#define`s, so there's nothing to sed.

   RGB status LED's own wizard mechanism (`rgbStatusLed` on
   `DEVICE_TYPES`, a `makeRgbStatusLed(redGpio, greenGpio, blueGpio)`
   factory) was fully removed along with the feature itself — see the
   "later removed" note above.
5. `firmware/addressable-light/` (WS2812B, 12-pixel strip, GPIO 2) taken
   through a real hardware test with Apple Home — the first time any
   ColorControl-bearing device type in this repo was tested against Apple
   Home rather than Home Assistant — and it initially failed completely:
   paired fine, but Apple Home showed it as a generic, unrecognized "Matter
   Accessory" ("Niet geschikt" / "Not compatible", no control tile at all),
   auto-removing its own fabric shortly after. Three real, independently
   confirmed bugs were found and fixed, all shared with
   `firmware/color-light/`'s identical hand-assembled ExtendedColorLight
   endpoint construction (both files were fixed together; only
   addressable-light was hardware-verified, since no RGB(W)(W) LED/driver
   board exists for color-light):
   1. **Missing `RemainingTime` attribute on ColorControl** — a mandatory
      global attribute per the Matter spec, confirmed by reading
      esp-matter's own `endpoint::extended_color_light::add()` in
      `esp_matter_endpoint.cpp` directly: it always calls
      `color_control::attribute::create_remaining_time()` explicitly,
      separate from anything the individual feature `add()` functions set
      up. This hand-assembled endpoint never called it at all.
   2. **Only the optional HueSaturation ColorControl feature was
      implemented — XY and ColorTemperature, both *mandatory* conformance
      for the ExtendedColorLight device type, were missing entirely.**
      Confirmed directly against the CSA's own
      `data_model/1.6/device_types/ExtendedColorLight.xml` (fetched from
      inside the esp-matter SDK image): `<feature code="XY">
      <mandatoryConform/>`, `<feature code="CT"><mandatoryConform/>`,
      HueSaturation itself only `<optionalConform/>`. The original
      HS-only design (deliberately choosing HS over esp-matter's own
      XY+CT default, reasoning that most controllers' color wheels drive
      Hue/Saturation directly) passed Home Assistant's lenient Matter
      integration but not Apple's. Fixed by adding real XY support (a new
      `xy_to_rgb()` — Philips' own published Hue CIE-xyY-to-sRGB
      conversion algorithm, the same one Home Assistant's color utility
      uses) and real ColorTemperature support even on the plain RGB/RGBW
      chips that have no dedicated warm/cool white channel (a new
      `mireds_to_rgb_approx()` — Tanner Helland's widely-used
      blackbody-radiation Kelvin-to-RGB approximation, the same technique
      WLED uses for this exact "no physical white LED" case) — RGBCCT
      chips (WS2805/SM2335EGH, and color-light's RGBWW mode) keep driving
      their real warm/cool channels for CT as before. `light_color_source`
      became a 3-way HS/XY/CT interlock (was 2-way HS/CT, and only existed
      at all for the RGBCCT chips) tracking whichever color space a
      controller most recently commanded.
   3. **The real root cause, found last: the endpoint had no Descriptor
      cluster at all.** Confirmed by reading esp-matter's own
      `common::create<T>()` template (used internally by *every* top-level
      endpoint helper — `endpoint::on_off_light::create()`,
      `endpoint::contact_sensor::create()`, `endpoint::extended_color_light
      ::create()`, all of them) directly in `esp_matter_endpoint.cpp`: it
      always calls `descriptor::create()` explicitly, before the device
      type's own `add()` runs. `firmware/color-light/` and
      `firmware/addressable-light/` hand-assemble their endpoint from raw
      `endpoint::create()` + individual `cluster::xxx::create()` calls
      instead (see each file's own header comment for why — to get
      HueSaturation instead of esp-matter's XY+CT default), which skipped
      this step entirely — the one thing every *other* device type in this
      repo gets for free by using a complete top-level helper.
      `add_device_type()` does NOT create or populate a Descriptor cluster
      itself (confirmed by reading its implementation: it only appends to
      the endpoint's own internal `device_types[]` array) — without an
      actual Descriptor cluster object on the endpoint, a controller has no
      standard way to discover what device type or clusters even exist
      there at all. This explains every symptom observed: commissioning
      itself doesn't touch Descriptor so it always succeeded (confirmed via
      a live serial log across every attempt — clean `CommissioningComplete`,
      `UpdateFabricLabel`, no protocol errors), but Apple Home's
      HomeKit-Matter bridge apparently can't (or won't) expose *any*
      service without one, while Home Assistant's more lenient Matter
      implementation tolerated the gap — which is exactly why this was
      never caught by this repo's existing Home Assistant-based testing.
      Fixed by explicitly creating a `cluster::descriptor::create(ep,
      &descriptor_config, CLUSTER_FLAG_SERVER)` right after
      `endpoint::create()`, before `add_device_type()` and every other
      cluster — matching `common::create<T>()`'s own order exactly.

   Debugging this took four full flash/pair/inspect cycles before finding
   the Descriptor cluster gap, including two that used a completely fresh
   factory-partition identity (new UUID/serial/discriminator via
   `tools/gen_factory.sh`) to rule out an Apple-side compatibility cache as
   the explanation for the identical failure repeating — a real, useful
   process reminder: `tools/gen_factory.sh` is meant to be re-run per
   physical unit rather than reusing one factory partition's `out/`
   output across multiple flashes, both for real deployments (every real
   product needs its own identity) and for exactly this kind of hardware
   debugging (ruling out identity-keyed caching as a variable). Each
   fix was isolated and confirmed via a live, unbuffered pyserial log
   read directly from the board during actual commissioning attempts
   (see the `hardware-test-setup` memory) rather than guessed at from
   Apple's opaque UI alone — the log showed the exact moment each
   commissioning attempt succeeded or got torn down
   (`OpCreds: Received a RemoveFabric Command`), which is what made it
   possible to tell "commissioning failing" apart from "commissioning
   succeeding but the resulting endpoint being rejected afterward" (it
   was always the latter).

   Once fixed: end-to-end confirmed live via Apple Home on real hardware
   (WS2812B, 12 pixels, GPIO 2) — Hue/Saturation, ColorTemperature (mireds),
   and CurrentLevel (brightness) all exercised live via the Home app's
   color wheel/color-temperature slider/brightness slider, every single
   command visible in the serial log reaching `app_attribute_update_cb()`
   and correctly updating the physical strip's color/brightness in real
   time, confirmed visually against the actual LEDs. This is also the
   first confirmation that this repo's XY-to-RGB and CCT-to-RGB
   approximation math (both newly added) produce visually correct,
   sensible-looking colors on real hardware, not just numerically
   plausible ones.

   Separately, also fixed while investigating: Apple Home proposed the
   generic "Matter Accessory" as this device's suggested name during
   pairing (rather than anything specific) because the `NodeLabel`
   attribute (Basic Information cluster, root endpoint) was left at
   esp-matter's own empty default — both files now set
   `node_config.root_node.basic_information.node_label` to a real default
   ("Addressable Light" / "Color Light") — cosmetic (a controller can
   always rename it) but a real, easy, worthwhile fix found in the same
   sitting.

   `firmware/color-light/` itself remains build-verified only for all
   three color modes (RGB/RGBW/RGBWW) with this same set of fixes applied —
   not hardware-tested (no RGB(W)(W) LED/driver board for this device type
   physically available), so its XY/CT rendering in particular (verified
   working on addressable-light's WS2812B path) hasn't been visually
   confirmed there yet.

   A fourth real bug was found in the same hardware-testing session, this
   time from ordinary live use rather than a compatibility failure:
   dimming the strip to a low level (e.g. 21%), turning it off, then back
   on caused it to jump to a fixed ~50% instead of staying at 21%. Root
   cause, confirmed by reading connectedhomeip's own
   `src/app/clusters/level-control/codegen/level-control.cpp`
   (`emberAfOnOffClusterLevelControlEffectCallback()`) directly: all three
   of `firmware/addressable-light/`, `firmware/color-light/`, and
   `firmware/dimmable-light/` set LevelControl's `OnLevel` attribute to a
   *concrete* default level (`..._DEFAULT_LEVEL`, 128 ≈ 50%) instead of
   leaving it null. Per that handler, whenever OnLevel is non-null,
   CurrentLevel is forced to OnLevel on every plain OnOff::On — the SDK
   doing exactly what it was told, just not what any real dimmable light
   should do. With OnLevel null, the same handler instead falls back to
   restoring whatever level was in effect right before the light went
   off — the "remembers your last brightness" behavior every real
   dimmable light has. Fixed in all three files by constructing
   `nullable<uint8_t>()` (confirmed, by reading
   `esp_matter_attribute_utils.h` directly, that the no-argument
   constructor actually produces a null value, not just a
   zero-initialized one) instead of passing a concrete level. Rebuilt and
   reflashed to the same addressable-light hardware and confirmed live:
   dimming down, turning off, turning back on now correctly restores the
   pre-off brightness rather than jumping to a fixed default.
   `firmware/color-light/` and `firmware/dimmable-light/` get the
   identical fix, Docker build-verified; only addressable-light was
   re-flashed and hardware-confirmed (it's what was on the bench).

   `firmware/dimmable-light/`'s own copy of the same fix was then also
   confirmed on real hardware, in a follow-up session on the same rig — a
   fresh factory-partition identity (own `gen_factory.sh` run, per-device
   as intended — see below), full commission via Apple Home, then dim to
   ~26% (67/254), turn off, turn back on: the live serial log showed
   `Light turned ON` immediately followed by `Light level set to 67/254`,
   i.e. the same "remembers your last brightness" restore already
   confirmed on addressable-light, not the old ~50% jump. This
   confirmation is log-based only, not visual — GPIO 2 on this rig still
   had the addressable strip's DATA line wired to it from the prior test,
   and a plain PWM duty cycle (what dimmable-light drives) is not a valid
   WS2812B/SK6812 bitstream, so the strip predictably stayed dark
   regardless of what the firmware was doing internally; a real visual
   confirmation would need a plain LED (+ resistor) wired to GPIO 2
   instead. Also encountered and worth remembering for future hardware
   sessions: Apple Home's app itself got stuck twice during this same
   session, in each case only sending an `Off` command repeatedly (never
   the `On` the user was actually tapping) until the Home app was fully
   force-quit and reopened — confirmed as a client-side quirk, not a
   firmware issue, by reading the live serial log directly (the firmware
   correctly processed every command it was actually sent; no `On`
   command reached the device at all until the app was restarted).
6. A real, previously-undetected bug in `firmware/fan/` and `firmware/
   air-purifier/`: both called `FanControl::SetDefaultDelegate()` BEFORE
   `esp_matter::start()`, and it was a silent no-op the whole time. Found
   while researching a new device type (`firmware/valve/`, whose
   `ValveConfigurationAndControl` cluster uses the exact same
   `SetDefaultDelegate()`-free-function pattern as FanControl) and reading
   esp-matter's own `fan_control/integration.cpp` closely enough to notice
   `SetDefaultDelegate()`'s own guard clause:
   `VerifyOrReturn(it != gServers.end() && it->second.server.IsConstructed())`.
   That map entry is only populated inside
   `ESPMatterFanControlClusterServerInitCallback()`, which fires as part
   of `chip::Server::GetInstance().Init()` — called from
   `esp_matter::start()` itself (confirmed directly in
   `esp_matter_core.cpp`), not from `endpoint::fan::create()` earlier in
   `app_main()`. So every call to `SetDefaultDelegate()` before `start()`
   found no map entry at all and returned immediately, meaning the real
   `FanDelegate` was never actually attached in either file: FanControl
   still accepted and stored `PercentSetting` writes internally (via its
   own wrapper, which tolerates a null wrapped delegate, so a controller
   saw no error), but `OnFanDriveStateChanged()` never fired — the
   physical PWM output never moved, and `PercentCurrent` never updated to
   reflect it. This went undetected because neither device type has been
   hardware-tested yet; Docker build-verification only confirms
   compilation, not this kind of runtime wiring order. Fixed in both
   files by moving `SetDefaultDelegate()` to after the
   `esp_matter::start()` call succeeds — confirmed by rebuilding both in
   Docker (clean compiles) — with an inline comment at the new call site
   in each file explaining the ordering requirement, matching the same
   "code-driven cluster's real construction happens inside
   `esp_matter::start()`, not at creation time" lesson this session
   already learned three times over for FeatureMap specifically
   (AirQuality, BooleanState, ResourceMonitoring) — this is the same
   underlying timing model applied to a `SetDefaultDelegate()`-style free
   function instead of a raw attribute write. Worth remembering as a
   general rule for any future Delegate registered via a similar
   esp-matter-provided free function: always call it after
   `esp_matter::start()`, never before, unless that specific function is
   confirmed (by reading its own implementation, not assumed) to handle
   the not-yet-constructed case some other way. `firmware/valve/` (added
   immediately after this fix, see below) got the correct ordering from
   the start.

## Note on hardware/USB

Building happens in Docker; flashing happens on the host with `esptool`. On Linux
you can alternatively pass the device into the container
(`--device=/dev/ttyUSB0`) and use `idf.py flash`, but the host route works on
macOS/Windows/Linux alike.
