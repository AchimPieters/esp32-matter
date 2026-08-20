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

`firmware/color-light/` is a ninth example — an RGB/RGBW/RGBWW light,
still declared as the `ExtendedColorLight` Matter device type but
implementing only Hue/Saturation (plus ColorTemperatureMireds in RGBWW
mode, see below), not esp-matter's `endpoint::extended_color_light
::create()` default of Xy + ColorTemperature for every build (confirmed
in esp-matter's own source that helper never actually adds HueSaturation
at all, despite that being what most controllers' color wheels drive
first). Built by calling esp-matter's own lower-level free functions
directly (`endpoint::create()`, `add_device_type()`, each cluster's own
`create()`/`feature::xxx::add()`) rather than the higher-level endpoint
helper — the same public API that helper is itself built from, just
composed to skip the CIE xyY color mode this repo isn't implementing a
conversion for. OnOff/LevelControl/ColorControl's Hue and Saturation
attributes are all plain ember attributes (confirmed against esp-matter's
own `examples/light/main/app_driver.cpp`) — no Delegate needed. Three
LEDC PWM channels (R/G/B, one shared timer) combine with LevelControl's
brightness through a standard HSV→RGB conversion.

`COLOR_LIGHT_COLOR_MODE` selects one of three build-time hardware
variants. RGBW adds a 4th channel driving an RGBW LED/strip's separate
white output, derived from the same Hue/Saturation color via the
standard "extract common white" technique (`W = min(R,G,B)`, then
subtract `W` from each of R/G/B) — the same approach Home Assistant's own
color utility and WLED use, not something invented for this file;
Matter's ColorControl cluster itself has no "white" concept, this is
purely a local hardware-rendering choice. RGBWW (sold by LED strip
vendors as "RGBCCT"/"RGB+CCT" — same 5-channel hardware) adds separate
cool-white and warm-white channels instead, and is a genuinely different
case: real RGBCCT hardware doesn't blend RGB and white simultaneously
(the same "color_interlock" ESPHome's own rgbww light component
documents), so this variant adds Matter's ColorTemperature feature
alongside HueSaturation and locally tracks which color space a
controller most recently commanded — driving either the RGB channels or
the cool/warm channels, never both. The mireds→channel-duty conversion
reuses ESPHome's own `light_call.cpp` formula (clamp into range,
linear-interpolate the warm/cool fraction, normalize both by their max so
one channel always stays at full strength). See its `app_main.cpp`
header comment for the full explanation of all three variants. Build-
verified in Docker for RGB (default), RGBW, and RGBWW; not
hardware-tested (no RGB(W)(W) LED/driver board physically available when
written).

`firmware/addressable-light/` is a tenth example — the same Matter
capability as `firmware/color-light/` (one Hue/Saturation/brightness
color, plus color temperature for its two RGBCCT chips), but driving an
addressable strip or driver chip instead of plain PWM channels. Every
pixel/fixture is always set to the same color: Matter has no ratified way
to command anything else (its `DynamicLighting` cluster, which would
cover per-pixel/gradient effects, is still `provisional` and absent from
every shipped Matter spec version — confirmed directly in
connectedhomeip's own `controller-clusters.matter`), so this is a
different physical layer for the same single-color light, not "RGBIC"
per-zone control. `ADDRESSABLE_LIGHT_CHIP` selects from eight chips across
three protocol families: six single-wire NRZ chips driven via ESP-IDF's
`driver/rmt_tx.h` (WS2812B, WS2813, WS2815, SK6812, SK6812 RGBW, and
WS2805 — the last a genuine 5-channel RGBCCT chip using the same
ColorTemperature/interlock design as Color Light's own RGBWW mode);
APA102 (DotStar), a real 2-wire SPI protocol driven via
`driver/spi_master.h`; and SM2335EGH, a bit-banged 2-wire smart-bulb
driver chip that — unlike every other chip here — drives one fixture's
RGB+CW+WW channels directly rather than a chain of pixels, so the pixel
count field doesn't apply to it. Every chip's protocol was independently
verified against its own manufacturer datasheet where one exists
(Worldsemi, fetched as PDFs and read with `pdftotext`) or the best
available community-verified source where it doesn't (APA102, SM2335EGH
— neither has a usable official protocol datasheet). Identify defaults to
flashing the strip/fixture itself rather than a separate LED. See its
`app_main.cpp` header comment for the full explanation of every chip's
sourcing. Build-verified in Docker for all 8 chips; not hardware-tested
(none of the 8 chips' hardware was physically available when written).

`firmware/thermostat/` (Heat + Cool) is this repo's newest, eleventh
device type — a genuine control loop (local temperature vs. a setpoint)
with a choice of three ways to actually reach a boiler/AC: direct relay
wiring, a bound remote relay module (Matter's own Binding cluster), or a
full native OpenTherm master, plus an optional rotary encoder and a
choice of three local displays (GC9A01/ST7789 TFT, SSD1306 OLED). See
CLAUDE.md's repository-layout entry for the full technical detail and
sourcing. Docker build-verified across every output-mode/encoder/display
combination; not yet hardware-tested (none of this device type's
hardware was physically available when written).

`firmware/camera/` (Matter Camera) is this repo's twelfth device type,
and unlike every other one, not something to copy-and-adapt: it's a
verbatim copy of esp-matter's own reference camera example (Public
Domain/CC0), needed because real Matter Camera (`WebRTCTransportProvider`
+ `CameraAvStreamManagement`, live WebRTC video) requires simultaneous
Matter signaling and real hardware video encoding — more than any single
chip elsewhere in this repo can do. It's a two-chip split architecture
(ESP32-P4 for camera/video encode + ESP32-C6 for Matter, one physical
**ESP32-P4 Function EV Board**) where `firmware/camera/` is only the
ESP32-C6 signaling half, and needs a real external SDK dependency (the
Amazon Kinesis Video Streams WebRTC SDK, cloned separately) that this
repo doesn't bundle. See `firmware/camera/README.md`'s own preamble and
CLAUDE.md's repository-layout entry for the full detail. Genuinely
Docker-build-verified with the real external SDK cloned and built
against it; not hardware-tested (no ESP32-P4 Function EV Board was
available), and not offered in the product wizard at all — its
one-chip-one-firmware data model can't represent this honestly.

`firmware/door-lock/` (Matter Door Lock) is this repo's thirteenth device
type, back to the normal one-chip/one-firmware pattern after camera's
exception. It's this repo's first device type where the main command
(LockDoor/UnlockDoor) is handled through a plain C weak-symbol override —
`emberAfPluginDoorLockOnDoorLockCommand()`/`OnDoorUnlockCommand()` —
rather than either the `attribute::PRE_UPDATE` pattern or a C++ Delegate
class used elsewhere, the documented extension point connectedhomeip
itself calls when no Delegate is configured. `DOOR_LOCK_OUTPUT_TYPE`
offers SERVO (default — a hobby servo retrofitting an existing
thumb-turn deadbolt) or RELAY (an electric strike/solenoid); an optional
position sensor lets LockState reflect a real reading instead of the
spec-allowed optimistic default. See CLAUDE.md's repository-layout entry
for the full technical detail, including two real build failures (one
compile, one link) an actual Docker build caught along the way. Docker
build-verified across servo/relay and with/without the position sensor;
not yet hardware-tested (no servo/relay/reed-switch hardware for this
device type was physically available when written).

`firmware/smoke-co-alarm/` (Matter Smoke/CO Alarm) is this repo's
fourteenth device type — its first over the SmokeCoAlarm cluster, a real
life-safety alarm class rather than a plain sensor readout. An MQ-2 smoke
sensor, an MQ-7 CO sensor, or both together (the default, matching how
real combination smoke+CO alarms are sold) drive the cluster's
SmokeState/COState through a plain adjustable-millivolt-threshold
classifier — deliberately not a calibrated ppm reading, since MQ-series
datasheets only document ppm as curves that shift per sensor/module, and
Matter's own cluster has no numeric concentration attribute to report one
into anyway. A real controller's SelfTestRequest command is fully
supported, including a genuine SDK gap this file works around: the
cluster sets `TestInProgress=true` on its own with no Delegate needed,
but nothing clears it afterwards unless the app does — confirmed by
reading `SmokeCoAlarmCluster`'s own source rather than assumed. See
CLAUDE.md's repository-layout entry for the full detail. Docker
build-verified across all 3 sensor configs (MQ2+MQ7, MQ2-only,
MQ7-only); not yet hardware-tested (no MQ-2/MQ-7 module was physically
available when written).

`firmware/occupancy-sensor/` (Matter Occupancy Sensor) is this repo's
fifteenth device type — a motion module reporting occupied/unoccupied via
the OccupancySensing cluster, with a choice of three sensors
(`OCCUPANCY_SENSOR_TYPE`): PIR (default, e.g. HC-SR501), RCWL-0516
(microwave Doppler radar), or HLK-LD2410 (24GHz mmWave presence radar —
only its simple digital OUT pin is used, not its richer UART protocol).
All three share the exact same GPIO interface, confirmed per chip against
real sourcing. Built using esp-matter's own complete
`endpoint::occupancy_sensor::create()` top-level helper rather than
hand-assembling clusters — deliberately, since that's exactly what
sidesteps the missing-Descriptor-cluster bug `firmware/color-light/` and
`firmware/addressable-light/` were found to have during this repo's own
Apple Home hardware testing (see CLAUDE.md's "Open next steps" for the
full story). Docker build-verified for all three sensor types; only PIR
is hardware-verified end to end: commissioned via Apple Home with zero
errors, then real PIR motion correctly flipped the Home app's tile live.

`firmware/fan/` (Matter Fan) is this repo's sixteenth device type and its
second genuine Delegate-based cluster after Window Covering's
WindowCovering — real PWM speed control (0-100%, PercentSetting/
PercentCurrent only, no MultiSpeed/Auto/Rocking/Wind/Step/
AirflowDirection) driving a MOSFET or fan-speed-controller board via the
same LEDC peripheral `firmware/dimmable-light/` uses for brightness.
Built with esp-matter's own complete `endpoint::fan::create()` top-level
helper, same "avoid the missing-Descriptor-cluster bug class" precedent
as `firmware/occupancy-sensor/`. Landing the right way to report
PercentCurrent back to the cluster took two real, sequential Docker build
failures — a compile error, then a link error — root-caused by reading
esp-matter's own `fan_control/integration.cpp` directly rather than
trusting connectedhomeip's generic header alone: esp-matter's build
substitutes its own integration.cpp, which only implements
`SetDefaultDelegate()`, not the `Attributes::PercentCurrent::Set()` free
function the generic header declares — fixed with the same registry-
lookup-and-cast pattern `firmware/contact-sensor/` and
`firmware/occupancy-sensor/` already use. See CLAUDE.md's repository-
layout entry for the full detail. Docker build-verified; not yet
hardware-tested (no PWM fan/MOSFET driver board was physically available
when written).

`firmware/air-quality-sensor/` (Matter Air Quality Sensor) is this repo's
seventeenth device type — a CCS811 I2C gas sensor reporting real
calibrated eCO2 (ppm) and eTVOC (ppb) via
CarbonDioxideConcentrationMeasurement/
TotalVolatileOrganicCompoundsConcentrationMeasurement, plus a Good/Poor
headline via the AirQuality cluster, all on one endpoint — confirmed as a
legitimate combination directly against the CSA's own
AirQualitySensor.xml before writing any code. A real esp-matter gap was
found and deliberately scoped around: `air_quality::create()` hardcodes
FeatureMap to 0 with no `config->feature_flags` field at all (unlike
every comparable optional-feature cluster in this repo), so only
AirQuality's base Good/Poor/Unknown scale is reachable through the
helper today — the finer Fair/Moderate/VeryPoor/ExtremelyPoor states are
a documented future step rather than an unverified workaround. CCS811's
protocol (I2C address, nWAKE-to-GND wiring, boot sequence, register map,
output ranges) was verified directly against ams's own datasheet and
Programming Guide, fetched as PDFs and read via `pdftotext`. See
CLAUDE.md's repository-layout entry for the full detail. Docker
build-verified; not yet hardware-tested (no CCS811 module was physically
available when written).

`firmware/water-leak-detector/` (Matter Water Leak Detector) is this
repo's eighteenth device type, and the closest sibling to
`firmware/contact-sensor/` here — esp-matter's own `water_leak_detector::
add()` and `contact_sensor::add()` are structurally identical (Identify +
BooleanState + StateChange event). The one thing that genuinely differs:
StateValue means the OPPOSITE thing here — `true` = "leak detected", not
contact-sensor's `true` = "closed" — confirmed against Espressif's own
`MatterWaterLeakDetector` Arduino-ESP32 API and Apple's HomeKit Leak
Sensor characteristic direction before writing any code. Also surfaced
the same class of esp-matter FeatureMap gap the air quality sensor found
(`boolean_state::create()` never sets the ChangeEvent feature its own
spec requires) — but unlike that one, this gap was judged safe to fix
directly (a plain `attribute::update()` on FeatureMap before
`esp_matter::start()`) rather than just documented, since the underlying
event fires unconditionally either way. See CLAUDE.md's repository-layout
entry for the full detail. Docker build-verified; not yet hardware-tested
(no water sensor module was physically available when written).

`firmware/air-purifier/` (Matter Air Purifier) is this repo's nineteenth
device type — a direct extension of `firmware/fan/`: same FanControl
Delegate, PWM output, and scope reused near-verbatim, plus
HepaFilterMonitoring and ActivatedCarbonFilterMonitoring on the same
endpoint, the two clusters that actually make it an air purifier rather
than a plain fan. `resource_monitoring::create()` has the same
FeatureMap-hardcoded-to-0 gap air-quality-sensor and water-leak-detector
both found — but here esp-matter exposes a real, public
`feature::condition::add()` API to enable it properly, plus a ready-made
`GetClusterInstance()` free function for updating Condition/
ChangeIndication at runtime, a fifth genuinely distinct pattern in this
repo for writing code-driven cluster attributes from app code. Filter
life is a plain, adjustable time-based estimate — accumulated
fan-running seconds against each filter's own configurable rated life in
hours, persisted across reboots — not a real sensor reading. See
CLAUDE.md's repository-layout entry for the full detail, including two
real compile errors an actual Docker build caught along the way. Docker
build-verified; not yet hardware-tested (no PWM fan/MOSFET driver board
was physically available when written).

To add another (simpler) type, copy any of the other eighteen folders
and swap the endpoint type in `app_main.cpp` — esp-matter provides many
more ready-made types, e.g. `robotic_vacuum_cleaner`.

## Updates

No CI yet (see CLAUDE.md — an automated build/release workflow was tried
but the multi-GB Docker image stalled out on GitHub-hosted runners, so it
was reverted rather than left flaky). Build + flash a new version yourself
following the Quick Start steps above whenever you change `app_main.cpp`.

All nineteen device types also ship with Matter's **OTA Requestor** cluster
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

All nineteen device types also have a quick-power-cycle factory reset
(see CLAUDE.md's "Open next steps" for the full sourcing/verification
detail): power the device off and on 3 times in a row (about 2 seconds
each way) and it factory-resets and re-enters setup mode, no button or
extra pin needed. Built on esp-matter's own `esp_matter::factory_reset()`,
called only after Matter has started (confirmed against its own
implementation and reference `app_reset` component).

Docker build-verified across all nineteen device types, not yet
hardware-tested. The product wizard (`tools/product-wizard/`) shows the
factory-reset procedure as a standalone info box under Configuration
Summary. (An earlier optional RGB status LED feature was built and
extended across every device type here too, then removed — see
CLAUDE.md's "Open next steps" for why.)

## Honest expectations

This is a genuine firmware project, not a no-code app. You work from the command
line with ESP-IDF and C++ (inside Docker, so no local toolchain mess). That's the
price of "no hidden code, nothing shared" — in return you own and can read every
part of it.

## License

MIT — see `LICENSE`.
