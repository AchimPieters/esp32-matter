# Product Wizard

A local, offline, no-build web UI for setting up esp32-matter products —
loosely inspired by Espressif's ESP ZeroCode wizard, adapted to this
project's local-first principles (no cloud, no telemetry, nothing leaves
your machine).

## Run it

No server, no build step — just open the file:

```bash
open tools/product-wizard/index.html
```

(or double-click it in Finder / drag it into a browser tab).

## What's implemented

- **Dashboard** — lists products you've created (stored in the browser's
  `localStorage`, nothing is sent anywhere), each with a **&times;** button
  to delete it (with a confirmation prompt) without opening it.
- **Create a new product → Setup your product** — name your product and
  hit **Start**.
- **Get Started (step 1)** — pick a device type: "On/Off Light"
  (`firmware/light/`), "On/Off Switch" (`firmware/switch/`), "Contact
  Sensor" (`firmware/contact-sensor/`), "Outlet" (`firmware/outlet/`),
  "Temperature Sensor" (`firmware/temperature-sensor/`), "Light
  Sensor" (`firmware/light-sensor/`), "Dimmable Light"
  (`firmware/dimmable-light/`), "Window Covering"
  (`firmware/window-covering/`), "Color Light"
  (`firmware/color-light/`), or "Addressable LED Strip"
  (`firmware/addressable-light/`). All ten are real, buildable
  firmware, not just UI placeholders. Each card has its own hand-drawn
  line-art icon (`DEVICE_TYPE_ICONS` in `index.html`), in the spirit of
  Apple's own SF Symbols/HomeKit accessory icons — not the actual Apple
  assets (those are proprietary), but drawn to read the same way: light
  2.5px strokes, no background chrome baked into the icon itself (the
  card button it sits inside already has its own border, so an icon-level
  border too would double-frame it) — with one deliberate exception, the
  outlet icon, which got its own square wall-plate frame back after
  rendering the page with a headless Chromium (installed specifically to
  check this) and seeing that a bare circle-with-2-dots reads as a smiley
  face at real card size, not an outlet. The full technical description
  that used to be the only visible text is now the card's hover tooltip;
  the visible text is a short `shortDesc` (a handful of words) per entry.
- **Select Module (step 2)** — pick a target chip (ESP32, C2, C3, C5, C6,
  C61, S3, H2), mirroring what `tools/dev.sh` + `idf.py set-target`
  actually support. A small bordered "Matter" badge (every module here
  builds Matter firmware, so it's always shown) plus connectivity badges
  (Wi-Fi/BLE/Thread/Zigbee) reflect each chip's real radios — e.g.
  ESP32-H2 has no Wi-Fi. The "Zigbee" badge is deliberately shown more
  muted (lower opacity) than the other badges, and its hover tooltip
  explains why: C5/C6/H2's 802.15.4 radio can physically run Zigbee, but
  this repo/wizard only ever builds Matter firmware for it (via
  esp-matter, Matter-over-Thread) — never Zigbee — so the badge documents
  a hardware capability, not a feature this repo implements. Checked
  directly against ESP-IDF's own `soc_caps.h` per
  chip before adding any of this — ESP32-C61 has 802.15.4-capable
  silicon like C5/C6 on paper, but its `SOC_IEEE802154_SUPPORTED` define
  is commented out on this repo's pinned ESP-IDF version (v5.5.4), so it
  gets no Thread/Zigbee badge; ESP32-H4 and ESP32-H21 were left out of
  the module list entirely for the same reason (both have their radio
  capability defines commented out in this exact SDK version — not
  reliably usable here, unlike H2/C5/C6/C61's Wi-Fi/BLE); ESP32-P4 was
  left out because it has no Wi-Fi/BLE/802.15.4 radio of its own at all
  (needs an external companion radio chip); ESP32-S2 was left out because
  it has Wi-Fi but no BLE, so it can't do Matter's standard commissioning
  flow (esp-matter's own `light` example doesn't ship an S2 config
  either, for the same reason).
- **Configure Device (step 3)** — set the GPIO(s) each device type's
  driver actually exposes: the LED pin for the light, the button pin for
  the switch, the contact pin for the contact sensor, the output + button
  pins for the outlet, and pin 1/pin 2 for the temperature and light
  sensors (the device types needing a second GPIO — see `DEVICE_TYPES`'
  optional `secondary` field in `index.html`, alongside `driver` and
  `identify`; label-driven so it reads as "Button" for the outlet and
  "SCL" for a sensor's pin 2, not hardcoded either way). No PWM/dimming
  or debounce config yet, unlike the ESP ZeroCode screenshots this is
  modelled on. Defaults per module echo the comments in each
  `app_main.cpp`. Also an **Identify LED** checkbox, on by default —
  every device type has one, since it's a real Matter cluster (blinks in
  response to a controller's "Identify" command) implemented in every
  firmware file, not just a wizard-only option. Untick it and the wizard
  leaves that `#define` alone, so the firmware's shipped default GPIO
  stays in effect — the LED still exists in the compiled firmware either
  way, this only controls whether the wizard customises its pin. A
  "Configuration summary" sidebar mirrors the reference UI. Purely a
  value capture for now — it does **not** edit the firmware file yet;
  that's Generate Firmware's job.
  - Device types with `componentOptions` (temperature sensor, light
    sensor) show a **checkable hardware list** here instead of a plain
    dropdown: a bordered box of radio rows (one per sensor chip, each
    with its bus type and verified/unverified status) plus the selected
    sensor's detail note underneath. Replaced an earlier `<select>` after
    real user feedback that a dropdown didn't make each option's status
    ("SHT4x — I2C, unverified") visible at a glance the way a list can.
    Lives in its own sidebar column to the *left* of the main panel
    (`.config-sidebar`, same visual treatment as Configuration summary on
    the right — a `has-left-sidebar` modifier turns `.configure-grid`
    into a three-column layout for any device type with a checkable
    picker on this step), again from direct feedback: it originally
    lived inside the main panel as just another stacked `.driver-block`,
    which buried it below the fold next to fields that matter less.
    Collapses to a single column under 720px, with an intermediate
    two-column layout (list+main stacked above the summary) between
    720–960px so the page doesn't get uncomfortably narrow on a mid-size
    window. `has-left-sidebar` (renamed from an earlier
    sensor-specific `has-sensor-sidebar`) is shared with the switch's
    button-count picker below — same layout, same `.component-list`
    checkable-row styling, different content.
  - Real bug fixed alongside that redesign: for single-data-pin sensors
    (DHT11/DHT22/DS18B20's single-wire DATA line, the LDR's ADC pin —
    anything with `usesPin2: false` in `COMPONENT_LIBRARY`), the "pin 2"
    GPIO field used to still render as a normal, seemingly-required
    input labelled "SCL, I2C only" even though nothing in that sensor's
    driver reads it — reported directly as confusing ("als ik DHT22 kies
    heb ik geen pin 2 nodig?"). Now that field (and its row in the
    Configuration summary sidebar) is hidden outright whenever the
    selected component doesn't use it, replaced by a one-line "Not used
    by DHT22 (single data pin only)" note in the sidebar. The underlying
    `secondaryGpioPin` value is untouched — it's still defaulted and
    still gets written into `app_main.cpp` by Generate Firmware's sed
    command (harmlessly, since the driver never reads it) — this is a
    display-only fix, not a change to what firmware gets generated.
  - The Contact Sensor also got a `componentOptions` picker (Reed
    Switch, Hall-Effect Sensor, Microswitch), added on request purely
    for **visual/interface continuity** with the temperature and light
    sensors' pickers — explicitly *not* because it selects a different
    driver: unlike a temperature or light sensor chip, every contact-
    sensing option here is, from `app_main.cpp`'s point of view, just
    "read one GPIO, HIGH or LOW" — there's no protocol/command-set
    difference for a picker to actually switch between. Its
    `DEVICE_TYPES` entry deliberately has no `componentDefineName`, so
    `buildSedCommands()` never emits a component-related sed line for
    it — picking an option records a choice in the product but has zero
    effect on the generated firmware, unlike every other
    `componentOptions` device type. A new `componentsPurelyVisual: true`
    flag drives honest copy explaining exactly that in three places:
    the sidebar's detail note ("Cosmetic choice only..."), Customise &
    Review's summary row ("— cosmetic only, no effect on the generated
    firmware" instead of a verified/unverified claim), and Generate
    Firmware (a plain `field-note`, not a `warn-box`, since there's
    nothing to actually warn about). `COMPONENT_LIBRARY`'s per-component
    `verified` flag still applies (Reed Switch is the one physically
    tested in this repo; the other two aren't) — but framed as "which
    exact part has been tried," not "which driver has been tested," since
    here they're the same driver either way.
  - The On/Off Switch supports **1-4 independent buttons**, added
    directly on request ("meerdere buttons toevoegen met hun eigen
    gpio"): a "How many buttons?" checkable list (`extraButtons` +
    `buttonCountDefineName` on its `DEVICE_TYPES` entry) — same left
    sidebar, same checkable-row styling as the sensor-model picker above
    (originally a plain `<select>` inside the main panel, moved and
    restyled on the same direct feedback: "net als de Configuration
    summary... aan de linkerzijde... door middel van een tick") — shows
    0-3 additional per-button GPIO fields in the main panel on top of the
    always-present button 1. Each button becomes its own `on_off_light_switch` endpoint
    in `app_main.cpp`, independently bindable to a different target
    device — Matter's own way of modelling a physical multi-gang wall
    switch. Button 1 keeps using the pre-existing `gpioPin` product
    field (so switch products saved before this feature existed keep
    working, unchanged, defaulting to 1 button); buttons 2-4 use a new
    `buttonGpios` array field, only populated up to whatever
    `buttonCount` is currently set to — shrinking the count doesn't
    clear values for buttons beyond it, so growing it back later
    restores what was there instead of re-defaulting. This is the first
    device type needing a *variable* number of GPIO fields rather than a
    fixed one or two — `buttonCountDefineName` sets the plain-integer
    `#define SWITCH_BUTTON_COUNT` via the same non-`GPIO_NUM_` sed
    pattern `componentDefineName` already used for `SENSOR_TYPE`. Only
    the buttons actually enabled by the selected count get a sed line;
    the rest keep the firmware's shipped default GPIO, harmlessly (same
    "unused but harmless" pattern used elsewhere in this file). Flagged
    in Generate Firmware's warning box whenever more than one button is
    selected — only the original single-button configuration has been
    tested on real hardware so far, though the multi-button code path
    itself is build-verified in Docker up to 4 buttons and reuses the
    exact debounce/dispatch logic the single-button path already proved.
- **Test Product (step 4)** — a real Web Serial monitor (Chrome/Edge
  only, with a warning banner elsewhere). Connect to a board, pick a baud
  rate (115200 by default, matching `idf.py monitor`), and watch its live
  log — e.g. commissioning output. Works whether the board was flashed
  via Generate Firmware's commands (step 6) or some other way; it just
  monitors whatever's already on it. Testing is optional — Next is
  always enabled on this step.
- **Customise & Review (step 5)** — a review table (Product name, Device
  type, Module, Driver + IO pin, Identify LED — or "Not added" if you
  unticked it) with per-row **Edit** links that jump straight back to the
  relevant step, plus a **Generated configuration preview**: the actual
  `idf.py set-target <chip>` command and the exact `sed` command(s) that
  will edit `app_main.cpp` — one per enabled GPIO setting. A **Copy**
  button puts both on the clipboard. Next is disabled if any earlier step
  is incomplete.
- **Generate Firmware (step 6)** — two ready-to-paste commands, nothing to
  download:
  1. **Build + generate factory data (Docker).** Runs the same `sed`
     command(s) shown in Customise & Review, builds the firmware, then
     runs `tools/gen_factory.sh` to generate a factory partition + QR
     code — including a self-signed test attestation certificate the
     first time you run it, cached under `tools/test-credentials/` after
     that. Same pinned `espressif/esp-matter:release-v1.6_idf_v5.5.4`
     image and env setup as `tools/dev.sh` (see CLAUDE.md for why it's
     pinned, and why not ESP-IDF v6.0.x — esp-matter doesn't support that
     yet).
  2. **Flash everything (host).** An `esptool.py write_flash` command
     with the correct offsets for the chosen chip — including the
     bootloader offset, which differs between the classic ESP32 (`0x1000`)
     and every later chip (`0x0`) — plus `ota_data_initial.bin` (this
     partition table has no "factory" app slot, so the bootloader needs it
     to know which OTA slot to boot) and the factory partition, found via
     a shell glob since its filename includes a random UUID.

  The mount/file paths aren't placeholders: this page always lives at
  `<repo>/tools/product-wizard/index.html`, so it reads its own `file://`
  URL to fill in your actual checkout path. Still nothing is written to
  *this* repo automatically — you run both commands yourself.

  This step originally shipped applying a unified diff via `patch`,
  base64-embedded in the command. That was never actually run end to end
  before shipping — only its base64 round-trip was checked — and the
  first real attempt failed outright (`patch: **** Only garbage was found
  in the patch input.`, from the bare `@@` hunk header having no line
  numbers). Replaced with `sed` substitutions anchored on each
  `#define`'s name, which don't need line numbers or context lines at
  all, and this time verified by actually running the generated command
  against a scratch checkout.

- A second real bug, found later on real hardware rather than by
  inspection: switching device type on an already-configured product
  (e.g. Outlet → Temperature Sensor) left `gpioPin`/`secondaryGpioPin`/
  `identifyGpioPin` at the *previous* type's values — Configure Device
  only fills those in when they're `null`, so a stale non-null value
  silently stuck around showing the wrong GPIO number with no indication
  anything was wrong (a product switched from a driver-default-GPIO-2
  type kept showing GPIO 2 as the temperature sensor's SDA pin instead of
  the correct 21, even though the field's own hint text correctly said
  "GPIO 21 is a common default"). Fixed by resetting all the type-specific
  fields (now including `sensorModel` too) whenever the selected device
  type actually changes.

All six steps are implemented end to end: Dashboard → Setup → Get
Started → Select Module → Configure Device → Test Product → Customise &
Review → Generate Firmware. Five of the seven device types on classic
ESP32 have now been validated for real, through the wizard's own
generated commands run verbatim — built, factory data + QR generated,
flashed, and commissioned via Apple Home (full PASE/CASE handshake, no
errors). The light sensor has been built and boot-tested through the same
commands but not yet commissioned (see below). The dimmable light is
build-verified in Docker only — no board was free to flash when it was
added.

The switch commissions cleanly but then shows up in Apple Home as a
generic "Matter Accessory" / "Niet geschikt" (not compatible) tile with a
house icon — that's expected, not a bug: `on_off_light_switch` is a
CLIENT-only device type (see `firmware/switch/main/app_main.cpp`'s header
comment), so there's no server attribute for Apple Home to display or
control, and Apple/Google Home have no UI for setting up the Binding
cluster this device actually needs to do anything useful. Home Assistant
or `chip-tool` are the way to actually use it (see CLAUDE.md).

The outlet (`on_off_plug_in_unit`) was added specifically to give a
device type that *does* show up as a real, controllable tile — it does,
but as "Outlet"/"Stopcontact", not "Switch". That's also expected, not a
bug: checked directly against the Matter device type library
(`connectedhomeip/data_model/<version>/device_types/`) — every device
type with "Switch" in the name is a client/input device (same category as
`on_off_light_switch` above), none of them a controllable on/off output.
`on_off_plug_in_unit` is the spec-correct type for a self-contained on/off
device, icon included; see `firmware/outlet/main/app_main.cpp`'s header
comment for the full explanation.

The outlet later gained a **Power Monitoring** checkable-list picker in
Configure Device, stacked in the same left sidebar as the sensor-model/
button-count pickers: None, or one of six chips (BL0942, BL0937, HLW8012,
CSE7759, CSE7766, ADE7953). This uses a new `extraPickers` array on the
`DEVICE_TYPES` entry, deliberately kept separate from `componentOptions` —
reusing `componentOptions` here would have incorrectly fed the choice into
the GPIO-field-labeling logic (`driverLabel`/`secondaryFieldNeeded`) that
only makes sense for a sensor's own pin naming, not for a choice that
doesn't affect any GPIO field's label at all. Each `extraPickers` entry
independently drives its own sed line (`defineName`/`defineValue`, same
non-GPIO `#define`-selects-a-branch pattern `componentDefineName` already
used for `SENSOR_TYPE`), its own review row in Customise & Review, and its
own unverified-warning box in Generate Firmware — with an extra explicit
caveat singled out for ADE7953, since it's the least-certain of the six
drivers (see `firmware/outlet/main/app_main.cpp`'s header comment for
exactly why). Picking "None" is the default and has no build impact beyond
the one `#define`; picking a chip pulls in that chip's driver and a second
Matter endpoint (Electrical Sensor). None of the six chips has been tested
on real hardware here — flagged as such throughout, same standard as the
other build-verified-but-not-hardware-tested pieces in this repo.

An `extraPickers` entry for **Output** (Relay vs. LED) used to sit
alongside Power Monitoring here too, removed after real-world feedback:
offering LED as an equally-weighted choice next to Relay was misleading —
a relay is simply what an actual power outlet/smart plug switches with, so
`OUTLET_OUTPUT_TYPE` now always builds as the firmware's own default
(`OUTLET_OUTPUT_RELAY`), un-sed'd, same as any other `#define` this wizard
doesn't expose a field for. LED stays available by hand-editing the
`#define`, just not as a wizard picker — the mechanism this removal left
behind (a single-entry `extraPickers` array) is exactly why `extraPickers`
was designed as an array in the first place, not a fixed two-picker shape.

Each power-monitor `COMPONENT_LIBRARY` entry also carries a `pins` array —
its chip's own GPIOs (BL0942/CSE7766's UART RX/TX, BL0937/HLW8012/
CSE7759's shared SEL/CF/CF1 pulse pins, ADE7953's I2C SDA/SCL), each with a
label, `#define` name, and per-module default — so Configure Device shows
real, editable GPIO fields for whichever chip you actually pick, not just
the picker itself; a real gap in the first version of this feature, where
picking a chip gave no way to configure the pins it needs at all.
CSE7766 only exposes one field (RX) even though its `#define`s include a
TX pin too — the chip only transmits and this firmware only reads, so the
ESP32's own TX line goes nowhere useful, same "don't show a field the
driver doesn't use" principle as the temperature sensor's `usesPin2`.
HLW8012 and CSE7759 point at the exact same `PULSE_METER_*` `#define`
names as BL0937 (all three share one code path in `app_main.cpp`), so
their pin fields are deliberately identical, not chip-specific. There's
also a separate, independently-optional **Status LED** field/checkbox,
rendered directly above the required Identify LED's own checkbox (same
shape, defaulting off instead of on) for `OUTLET_STATUS_LED_GPIO` — some
real plug hardware has a small indicator LED, on its own GPIO, that
continuously mirrors on/off state, which is a different thing from
Identify (which only blinks temporarily on a controller's Identify
command). Deliberately placed above Identify, not below: on real hardware
the status LED is the one that reflects the device's actual, ongoing
state, so it reads better ordered ahead of the one-off Identify blink.

The temperature sensor (`temperature_sensor` + `humidity_sensor`, one
node with two endpoints — Matter has no single device type for both from
one sensor chip) supports 7 sensor chips (SHT3x, SHT4x, AHT20, DHT11,
DHT22, DS18B20, BME280), picked via a "Sensor model" dropdown in
Configure Device — this repo's first non-GPIO sensor and first device
with a choice of driver at all. Chosen to cover what's actually most
common from beginner through professional use (checked against current
sources, not assumed — an earlier draft only had the sensors already on
hand for testing, which nearly excluded BME280 despite it being the
ESP32/ESPHome/Home Assistant community's most-recommended all-rounder).
SHT3x, DHT11, and DHT22 are verified on real hardware; the other four are
implemented from their datasheets/reference drivers but not personally
tested here — the dropdown says so per sensor, and Generate Firmware
repeats the caveat for whichever one you pick. Verified against a
physical SHT3x: readings stayed stable even after moving the sensor away
from the ESP32 board (ruling out self-heating as the cause of an initial
offset from a cheap reference sensor nearby); SHT3x's spec'd accuracy
(±0.2 °C / ±2 %RH) is tighter than most low-cost reference sensors, so
that offset is most likely the reference's own inaccuracy, not a bug
here. DHT11 and DHT22 (sharing one bit-banged driver, just different byte
interpretation) worked correctly on the first flash each, no debugging
needed.

Adding this sensor-model choice needed a bit more than the outlet's
`secondary` GPIO field: a per-device-type component list plus a third sed
target (`#define SENSOR_TYPE ...`) alongside the two GPIO fields, since
which driver compiles in at all is now a real choice, not just a pin
number. Originally this list lived inline as each device type's own
`sensorModels` array; once a second device type could plausibly reuse the
same kind of component (e.g. a relay module usable by more than one
future actuator type), it was pulled out into a shared top-level
`COMPONENT_LIBRARY` dictionary keyed by component id, with device types
referencing it via `componentOptions: [id, ...]` and a `findComponent(id)`
lookup helper — `sensorModel` was renamed to the generic `component`
throughout to match. `firmware/temperature-sensor/` is still the only
device type actually using `componentOptions` today.

The light sensor (`firmware/light-sensor/`, `light_sensor` device type)
supports two sensor drivers, picked the same way the temperature sensor's
seven chips are: a "Sensor model" dropdown in Configure Device, driving a
`componentOptions: ["LDR", "BH1750"]` list on its `DEVICE_TYPES` entry
plus the same `#define SENSOR_TYPE ...` sed target. **LDR/photoresistor**
was the original, this repo's only analog/ADC driver (every other
sensor/type here is digital) — read through ESP-IDF's ADC oneshot +
calibration APIs, converted to lux via the standard photoresistor
characteristic curve. **BH1750** was added afterward: a digital ambient
light sensor over I2C (almost always sold as a "GY-30"/"GY-302"
breakout) that reports lux directly, no voltage-divider math or
per-unit LDR characterization needed. Both feed into the same Matter
logarithmic Illuminance Measurement encoding. Neither is hardware-tested
in this repo (no LDR or BH1750 module on hand) — both `SENSOR_TYPE`
values are build-verified in Docker; only the LDR path has been
boot-tested on real hardware (clean compile, clean boot, sane-looking
fallback readings with nothing wired to the ADC pin). Marked unverified
via `COMPONENT_LIBRARY`'s per-component `verified: false`, same mechanism
the temperature sensor's chips use — the light sensor originally had its
own device-type-level `hardwareVerified: false` flag instead, added back
when it only had one driver and no component list to attach a per-item
flag to; that flag was removed once a second driver made
`componentOptions` the better fit, and the whole-device-type warning
paths in Customise & Review / Generate Firmware went with it (the
per-component warning already covers the same case, one level more
precisely).

The dimmable light (`firmware/dimmable-light/`, `dimmable_light` device
type) is this repo's first device type with a real actuator beyond simple
on/off — added after being offered as one of three options (the others
were a color light and a window covering/blind) and picked as the
smallest reasonable step up from what already existed. Its `DEVICE_TYPES`
entry needed no new wizard mechanism at all: just one `driver` GPIO field
(the PWM-capable LED pin) plus `identify`, the exact same shape
`firmware/light/`'s own entry already uses — confirming that shape really
is generic across "any device with one main output pin," not something
that happened to only work for a plain digital output. Configure Device
labels the field "LED · PWM output (LEDC)" instead of "digital GPIO" to
make clear it needs an LEDC-capable pin (true for nearly every GPIO
except input-only ones), not any arbitrary GPIO the way a plain digital
output field does. Validated end to end on real hardware shortly after
being added — an ESP32 WROOM-32 with an LED on GPIO 2 (this device
type's default pin, so no `#define` edits were needed at all), built and
flashed via the wizard's own generated commands, commissioned into Home
Assistant (clean PASE/CASE handshake), then both On/Off and the
brightness slider exercised live from Home Assistant — confirmed via the
serial log, which showed a distinct `Light level set to N/254` line for
every step of a slider drag, matching what was actually done in the
controller's UI.

The window covering (`firmware/window-covering/`, `window_covering`
device type) is the option not picked when dimmable light was chosen from
the same three-way offer (the other being a color light) — this repo's
first device type with continuous, multi-second physical movement. Its
`DEVICE_TYPES` entry uses the same `driver` + `secondary` + `identify`
shape `firmware/outlet/`'s own entry already uses for a device type
needing two main GPIOs (here: an UP/open relay and a DOWN/close relay,
both active-LOW) — again no new wizard mechanism needed. Unlike every
other cluster this repo builds, WindowCovering's own Matter cluster
doesn't drive hardware or simulate movement by itself (confirmed directly
in esp-matter's source — it only validates commands and calls an
app-supplied Delegate), so the firmware needed its own timed-movement
logic: a shared FreeRTOS task drives the two relays and estimates
position via linear interpolation against a calibrated full-travel time,
with no position sensor assumed — the same technique ESPHome's/Tasmota's
own time-based cover components use. A real compile error was caught by
an actual Docker build, not by inspection: esp-matter's own `nullable<T>`
wrapper type isn't the same as `chip::app::DataModel::Nullable<T>`, and
the first build attempt used the wrong one. Build-verified in Docker; not
hardware-tested (no motor/relay hardware for this device type physically
available when written) — see `firmware/window-covering/main/app_main.cpp`'s
header comment for the full explanation.

The color light (`firmware/color-light/`, still `ExtendedColorLight`
device type) is the last of the same three-way offer — a color light
(the other option not picked). Implements exactly one ColorControl mode
(Hue/Saturation, RGB-only — no white channel), not esp-matter's own
`endpoint::extended_color_light::create()` default of Xy + color
temperature, confirmed in esp-matter's source to never actually add
HueSaturation despite that being what most controllers' color wheels
drive first — same "smallest reasonable next step" scoping as dimmable
light/window covering. Its `DEVICE_TYPES` entry reuses `extraButtons`
(previously switch-only) for the Green/Blue channels alongside `driver`'s
Red — but as a *fixed* set, not switch's variable 1-4 count. This
surfaced and fixed two real, previously-latent bugs in that shared
mechanism: it assumed a "how many?" picker and a `buttonCountDefineName`
would always be present (now gated behind a new
`hasVariableButtonCount` check, with per-entry `label` fields so summary
rows say "RED CHANNEL" instead of the switch-specific "BUTTON 1"
fallback), and `isProductComplete()` computed its own `buttonCount`
separately from Configure Device's, defaulting to 1 instead of a fixed
device type's true count whenever called before that step had ever
rendered — meaning a 3-channel product could have been reported
"complete" with 2 of 3 fields still unset. Both fixed, with a new smoke
test specifically calling `isProductComplete()` with no prior render to
catch a regression. Built by calling esp-matter's own lower-level free
functions directly rather than the higher-level endpoint helper — the
first device type in this repo assembled that way. Build-verified in
Docker; not hardware-tested (no RGB LED/driver board physically
available when written) — see `firmware/color-light/main/app_main.cpp`'s
header comment for the full explanation.

Color light then gained an optional RGBW mode
(`COLOR_LIGHT_HAS_WHITE_CHANNEL`, off by default) — a 4th LEDC channel
for an RGBW LED/strip's separate white output, computed from the same
Hue/Saturation color via the standard "extract common white" technique
(matches Home Assistant's own color utility and WLED, not invented for
this file). Rather than building new wizard mechanism for this, it
reuses the *existing* `extraPickers` + `COMPONENT_LIBRARY` `pins`
mechanism built for the outlet's power-monitoring chip picker: a named
option (`COLOR_MODE_RGB` / `COLOR_MODE_RGBW`) that, when picked, reveals
its own extra GPIO field and sed target — the same shape as "does this
outlet have a power-monitor chip, and if so which one." Zero new
render/validation/sed code was needed, only two new `COMPONENT_LIBRARY`
entries and one `extraPickers` entry on color-light's `DEVICE_TYPES`
record. Verified two ways: a Docker build for both RGB and RGBW
configurations (both clean), and a headless-Chromium screenshot of the
wizard's Configure Device step confirming the Color Mode picker, the
conditional White Channel GPIO field, and the configuration-summary row
all render correctly. Not hardware-tested (no RGB(W) LED/driver board
physically available when written).

A third mode, RGBWW (what LED strip vendors sell as "RGBCCT"/"RGB+CCT"
— separate cool-white and warm-white channels instead of RGBW's single
white channel), followed directly on the user's request. Unlike RGBW,
this isn't just "one more channel": real RGBCCT hardware never blends
RGB and white simultaneously (ESPHome's own rgbww light component docs
call this "color_interlock"), which maps onto Matter's ColorControl
cluster already treating Hue/Saturation and ColorTemperatureMireds as
separate `ColorMode` values — so this variant adds Matter's
ColorTemperature feature and the firmware locally tracks which color
space a controller last commanded, driving either the RGB channels or
the cool/warm channels, never both. This forced `COLOR_LIGHT_COLOR_MODE`
from a boolean flag into a real 3-way enum
(`COLOR_LIGHT_MODE_RGB`/`_RGBW`/`_RGBWW`) — safe since the RGBW-only
version had only just been committed, not yet released or hardware-
tested by anyone — and the `COMPONENT_LIBRARY` entries' `defineValue`
changed from raw `"0"`/`"1"` to the actual C constant names, matching
`SENSOR_TYPE`/`OUTLET_POWER_MONITOR`'s existing pattern. Despite adding
2 extra GPIO fields (cool white, warm white) instead of 1, this still
needed zero new render/validation/sed logic: the `pins` array mechanism
already handles any number of pins per option — confirmed by reading
that code path (it already drives BL0942's 2 UART pins and ADE7953's
SDA/SCL) rather than assuming it would need extending. Build-verified in
Docker for all three color modes; not hardware-tested (no RGB(W)(W)
LED/driver board physically available when written).

A tenth device type, `firmware/addressable-light/` ("Addressable LED
Strip"), followed directly on the user's request for WS2812B/SK6812
addressable-strip support. It's the same Matter capability as `Color
Light` — one Hue/Saturation/brightness color for the whole accessory —
over a genuinely different physical layer: a single-wire addressable
protocol (WS2812B/SK6812) driven via the ESP32's RMT peripheral, rather
than PWM. Explicitly documented as NOT per-pixel "RGBIC" control: Matter
does define a `DynamicLighting` cluster (0x0305) with exactly the
struct shapes real per-pixel effects would need, but it's marked
`provisional` in connectedhomeip's own `controller-clusters.matter` and
absent from every ratified spec version checked (1.0 through 1.6) — not
usable against any real, certified controller today, so this device
just drives every pixel to the same color, same as `Color Light`.
`ADDRESSABLE_LIGHT_CHIP` selects WS2812B (24-bit GRB) or SK6812 (32-bit
RGBW) — both chips' bit timing AND byte order independently verified
against Worldsemi's own datasheets (downloaded as PDFs and read via
`pdftotext` rather than trusted from search-engine summaries, which
surfaced two conflicting "official" WS2812B timing tables — the actual
document had to be read to resolve which). SK6812's datasheet-specified
RGBW byte order is flagged as disagreeing with several community
libraries' GRBW default — a real, documented caveat, not a guess. Uses
ESP-IDF's `driver/rmt_tx.h` — this repo's first RMT-based driver, with
the exact API pattern checked against Espressif's own official
`led_strip_simple_encoder` reference example rather than assumed. The
wizard's WS2812B/SK6812 chip picker reuses the same
componentOptions/componentDefineName mechanism the temperature/light
sensors' chip pickers already use, needing zero new wizard code —
confirmed by a Node.js sandboxed re-exec of the wizard's script and a
headless-Chromium screenshot of both chip selections' Configure Device
panels. Build-verified in Docker for both chips; not hardware-tested (no
WS2812B/SK6812 strip physically available when written).

Its pixel count (`ADDRESSABLE_LIGHT_PIXEL_COUNT`) was then made
wizard-configurable too — originally left as a hand-edit-only `#define`
since the wizard had no field type for a plain integer, only GPIO pins
and named enum choices. Rather than a one-off fix, this became a new
general-purpose `numberField` mechanism on `DEVICE_TYPES` entries
(`key`/`label`/`fieldLabel`/`blockTitle`/`defineName`/`defaultValue`/
`min`/`max`/`helpText`) — reusable by any future device type needing a
plain numeric `#define`, following the same "generalize it" precedent
`extraPickers`/`componentOptions`/`hasVariableButtonCount` set earlier.
Touched the same layers a GPIO field already does (render, validate,
sed generation, review summary, device-type-switch reset) plus a new
delegated `data-number-field` input handler mirroring `data-pin-define`'s
shape. Verified with a Node.js sandboxed re-exec and a headless-Chromium
screenshot across three states (default, an out-of-range value showing
the field error and disabling "Next step", and the review step's summary
row + generated sed command). One thing worth remembering for future
sandbox checks: the Node sandbox's fake `document.createElement` stub
initially didn't implement `textContent`/`innerHTML`, so every
`escapeHtml()` call silently returned `undefined` *inside the sandbox
only* — a test-harness gap, not a real bug, caught by cross-checking
against an actual Playwright screenshot before concluding otherwise.

`firmware/addressable-light/` then grew from 2 chips to 8, on request —
WS2812B, SK6812, SK6812 RGBW, WS2813, WS2815, APA102, then, mid-turn,
after the user shared screenshots of a real manufacturing/config tool's
"Device Drivers"/"Indicators" screens, WS2805 and SM2335EGH too. Two real
scope questions came up before writing any of it, resolved via
AskUserQuestion rather than assumed: whether the same chip list should
also appear on `Color Light` (it physically can't drive any of them —
plain PWM only — so the user chose a purely cosmetic reference picker
instead of real functionality there), and whether "5ch" should be a real
chip (none of the first 6 requested is actually 5-channel — the user
chose to have a genuine one researched, landing on WS2805). The wizard
side needed exactly two small, reusable additions rather than one-off
hacks: `hidesNumberField` on a COMPONENT_LIBRARY entry (SM2335EGH has no
pixel-chain concept at all, so the wizard hides the pixel-count field
when it's selected — checked everywhere `numberField` is read, the same
"opt out of a field this device type otherwise has" principle already
used for `usesPin2`), and `cosmetic` on an `extraPickers` entry (Color
Light's new reference-only chip list — renders normally but is skipped
entirely by `buildSedCommands` and given "reference only" framing instead
of "verified"/"build-tested" language, reusing the same contract
`componentsPurelyVisual` already established for contact-sensor's reed
switch/Hall sensor list rather than inventing a second mechanism for the
same idea). Building `hidesNumberField` surfaced two independently real,
pre-existing gaps, both fixed the same way: `renderCustomiseReview`'s
secondary-pin review row never checked `usesPin2` at all (would have
shown a misleading Clock-pin row for the 6 chips that don't use one), and
the sidebar's verified-hardware framing would have called a purely
cosmetic chip choice "not personally tested... build-verified only" —
true but misleading, since nothing is ever built from that choice.
Verified with a Node.js sandboxed re-exec (including secondary-field and
hidesNumberField checks across APA102/SM2335EGH) and headless-Chromium
screenshots of the default chip, APA102 (both pins), SM2335EGH (both
pins, pixel-count field correctly hidden), and Color Light's new cosmetic
picker at both the Configure Device and Customise & Review steps.

Reflashing a board that was previously commissioned with different
firmware/identity (as happened testing this, repeatedly, across several
of these device types) leaves stale fabric data in NVS — the write-flash
command only touches the bootloader, partition table, app, and `fctry`
partitions, not NVS, so the device comes up already "Operational" instead
of freshly commissionable. `esptool erase_flash` before reflashing gives
a genuinely fresh device; worth remembering for anyone re-purposing one
physical board across multiple wizard products like this repo's own
testing does.

Two more cross-cutting features followed, added to **all ten** device
types at once rather than staged — prompted by screenshots of a real
manufacturing/config tool's own "Indicators" and "Factory Reset" tabs,
which showed a much richer commissioning/Identify indicator state
machine and a power-cycle reset procedure neither existed here yet:

- **RGB status LED** — a new optional, independently-toggleable checkbox
  + 3 GPIO fields (red/green/blue) in Configure Device, off by default,
  rendered via a new `rgbStatusLed` mechanism on `DEVICE_TYPES`
  (`makeRgbStatusLed(redGpio, greenGpio, blueGpio)`, modeled on the
  outlet's existing single-pin `statusLed` above but generalized to an
  array of 3 pins — reusing the exact same per-component `pins` shape
  `extraPickers` already established for BL0942/ADE7953/APA102/SM2335,
  not a new mechanism). Wired into the same places every other optional
  GPIO block is: Configure Device's render/validate/default-fill/sed
  logic, the note box listing which `#define`s get applied, the
  Configuration summary sidebar (a new "RGB STATUS LED" row), Customise &
  Review, and the device-type-switch reset logic. On the firmware side
  this drives a real color + blink/breathe pattern engine reflecting
  commissioning state and Identify effects — grounded in connectedhomeip's
  own `CHIPDeviceEvent.h` lifecycle events and the Identify cluster's own
  `EffectIdentifierEnum`, not invented from the screenshot; see
  `firmware/light/main/app_main.cpp`'s header comment for the full
  state/color/timing table.
- **Factory reset info box** — a new static card rendered directly under
  the Configuration summary sidebar on every device type, describing the
  quick-power-cycle procedure every device type's firmware now actually
  implements (power off/on 3 times, ~2 seconds each way, then it
  factory-resets via `esp_matter::factory_reset()` and re-enters setup
  mode). Both boxes needed a small structural change to
  `renderConfigureDevice`'s markup: the summary sidebar and the new box
  are wrapped in a `.config-sidebar-stack` flex column so they stack as
  one grid item and stay in the same column regardless of whether that
  step's `.configure-grid` is in its 2-column or 3-column (left sidebar
  present) layout. Unlike the status LED, factory reset needed zero
  render/validate/sed wiring — it has no configurable GPIOs or
  `#define`s, the mechanism is always compiled in, so the box is purely
  informational.

Both features are Docker build-verified across all ten device types; not
yet hardware-tested.

## Known limitations

- Ten device types exist (`On/Off Light`, `On/Off Switch`, `Contact
  Sensor`, `Outlet`, `Temperature Sensor`, `Light Sensor`, `Dimmable
  Light`, `Window Covering`, `Color Light`, `Addressable LED Strip`) —
  light/switch/contact/outlet are all digital GPIO, temperature is this
  repo's first non-GPIO sensor (I2C, single-wire, or 1-Wire depending
  which of its 7 supported chips you pick), light sensor is the first
  analog/ADC one, dimmable light is the first with a real actuator
  beyond simple on/off (PWM output via LEDC), window covering is the
  first with continuous, multi-second physical movement (two relays +
  time-based position estimation, no position sensor), color light is
  the first with more than one PWM output channel driving one light
  (RGB/RGBW/RGBWW, up to 5 channels), and the addressable LED strip is
  the first over addressable/digital protocols rather than plain PWM —
  8 selectable chips across three families (six single-wire NRZ chips
  via RMT, APA102 over real SPI, and SM2335EGH bit-banged) — though,
  like color light, it only ever shows one uniform color across the
  whole strip/fixture, since Matter itself has no ratified per-pixel/
  effects concept (see its own `app_main.cpp` header comment). All three
  options from the original "next actuator" offer have now been built.
- The window covering has no position sensor of any kind — position is
  estimated purely from calibrated motor-on time (linear interpolation),
  the same technique ESPHome's/Tasmota's own time-based cover components
  use. This means the reported position can silently drift from reality if
  the motor stalls, slips, or the covering is moved by hand; only a full
  open or full close command re-anchors it to a known point (0% or 100%).
  Not a bug specific to this implementation — an inherent limitation of
  time-based (vs. sensor-based) position tracking, documented in
  `firmware/window-covering/main/app_main.cpp`'s header comment. Also not
  yet hardware-tested — no motor/relay hardware for this device type was
  physically available when it was built.
- The light sensor is the one device type in this list not actually
  confirmed against real hardware for both of its sensor options — no
  LDR or BH1750 module was on hand when either was built. Everything
  upstream of the physical sensor reading (build, factory data, flash)
  has been verified for both `SENSOR_TYPE` values; only the LDR path has
  additionally been boot-tested on a real board. `COMPONENT_LIBRARY`'s
  `verified: false` on both `LDR` and `BH1750` surfaces this in the
  Configure Device dropdown and the Generate Firmware warning box.
- The outlet's six power-monitoring chips are build-verified in Docker
  only, not tested against real hardware — none of the six modules was
  physically available here. Their protocols were checked against their
  own manufacturer datasheets rather than trusted from secondary sources,
  which caught two real bugs (BL0942 byte offsets, CSE7766 status-byte
  semantics) — but two of the six (CSE7759, ADE7953) could only be
  partially or indirectly verified even that way; see
  `firmware/outlet/main/app_main.cpp`'s header comment for exactly which
  chip got what level of verification. `COMPONENT_LIBRARY`'s per-chip
  `verified`/`note` fields surface this in the Power Monitoring picker
  and the Generate Firmware warning box, with an extra explicit caveat
  for ADE7953 specifically.
- The switch's buttons each send a real OnOff Toggle to whatever their own
  endpoint is bound to (`client::cluster_update()`), but those bindings
  themselves have to be set up through a controller with a Bindings UI
  (e.g. Home Assistant) — the wizard doesn't help with that part. Only the
  original single-button configuration (1 button, GPIO 4) has been tested
  on real hardware; 2-4 buttons is build-verified in Docker only so far —
  flagged in Generate Firmware's warning box whenever more than one
  button is selected.
- The generated `sed` commands match on the `#define`'s name, not its
  current value, so they're idempotent — but if a line's been hand-edited
  into some other shape entirely (not `#define NAME GPIO_NUM_<digits>`),
  the command silently no-ops instead of erroring. Customise & Review
  shows the exact command if you want to check or run it yourself.
- The flash command's offsets are only *physically verified* for
  esp32 — the other four modules follow documented ESP-IDF convention
  (see the comment above `MODULES` in `index.html`) but haven't been
  tested against real hardware in this repo. `idf.py build` always prints
  its own authoritative flash command at the end of step 1 — cross-check
  against that if one of them doesn't boot.
- Even with a correct, complete attestation chain, Apple Home / Google
  Home may still refuse to commission a device using this wizard's
  self-signed test certificate, depending on app/OS version — they
  validate against their own bundled root list, and a locally-generated
  test PAA is never on it. That's an ecosystem-level restriction, not a
  bug here (see the comment at the top of `tools/gen_factory.sh`). Home
  Assistant and chip-tool don't have this restriction.

## Design notes

- Single self-contained `index.html` — plain HTML/CSS/vanilla JS, no
  dependencies, matches the "no hidden code" principle in `CLAUDE.md`.
- State persists only in `localStorage`; clearing browser data resets it.
