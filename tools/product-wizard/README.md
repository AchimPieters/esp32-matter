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
  "Temperature Sensor" (`firmware/temperature-sensor/`), or "Light
  Sensor" (`firmware/light-sensor/`). All six are real, buildable
  firmware, not just UI placeholders.
- **Select Module (step 2)** — pick a target chip (ESP32 / C3 / C6 / S3 /
  H2), mirroring what `tools/dev.sh` + `idf.py set-target` actually
  support. Connectivity badges (Wi-Fi/BLE/Thread) reflect each chip's real
  radios — e.g. ESP32-H2 has no Wi-Fi.
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
Review → Generate Firmware. Five of the six device types on classic ESP32
have now been validated for real, through the wizard's own generated
commands run verbatim — built, factory data + QR generated, flashed, and
commissioned via Apple Home (full PASE/CASE handshake, no errors). The
light sensor has been built and boot-tested through the same commands but
not yet commissioned (see below).

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

The outlet later gained two more checkable-list pickers in Configure
Device, stacked in the same left sidebar as the sensor-model/button-count
pickers: **Output** (Relay default — active-LOW, what an actual power
outlet/smart plug normally switches with — or LED, active-HIGH, mainly for
breadboard testing without a relay on hand) and **Power Monitoring** (None,
or one of six chips: BL0942, BL0937, HLW8012, CSE7759, CSE7766, ADE7953).
These use a new `extraPickers` array on the `DEVICE_TYPES` entry,
deliberately kept separate from `componentOptions` — reusing
`componentOptions` here would have incorrectly fed these choices into the
GPIO-field-labeling logic (`driverLabel`/`secondaryFieldNeeded`) that only
makes sense for a sensor's own pin naming, not for a choice that doesn't
affect any GPIO field's label at all. Each `extraPickers` entry
independently drives its own sed line (`defineName`/`defineValue`, same
non-GPIO `#define`-selects-a-branch pattern `componentDefineName` already
used for `SENSOR_TYPE`), its own review row in Customise & Review, and its
own unverified-warning box in Generate Firmware — with an extra explicit
caveat singled out for ADE7953, since it's the least-certain of the six
drivers (see `firmware/outlet/main/app_main.cpp`'s header comment for
exactly why). Picking "None" for Power Monitoring is the default and has
no build impact beyond the one `#define`; picking a chip pulls in that
chip's driver and a second Matter endpoint (Electrical Sensor). None of
the six chips has been tested on real hardware here — flagged as such
throughout, same standard as the other build-verified-but-not-hardware-
tested pieces in this repo.

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
also a separate, independently-optional **Status LED** field/checkbox
(off by default, same shape as the required Identify LED's own checkbox)
for `OUTLET_STATUS_LED_GPIO` — some real plug hardware has a small
indicator LED, on its own GPIO, that continuously mirrors on/off state,
which is a different thing from Identify (which only blinks temporarily
on a controller's Identify command).

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

Reflashing a board that was previously commissioned with different
firmware/identity (as happened testing this, repeatedly, across several
of these device types) leaves stale fabric data in NVS — the write-flash
command only touches the bootloader, partition table, app, and `fctry`
partitions, not NVS, so the device comes up already "Operational" instead
of freshly commissionable. `esptool erase_flash` before reflashing gives
a genuinely fresh device; worth remembering for anyone re-purposing one
physical board across multiple wizard products like this repo's own
testing does.

## Known limitations

- Six device types exist (`On/Off Light`, `On/Off Switch`, `Contact
  Sensor`, `Outlet`, `Temperature Sensor`, `Light Sensor`) —
  light/switch/contact/outlet are all digital GPIO, temperature is this
  repo's first non-GPIO sensor (I2C, single-wire, or 1-Wire depending
  which of its 7 supported chips you pick), and light sensor is the first
  analog/ADC one. A device type with a physical actuator beyond simple
  on/off (e.g. a dimmable/color light, or a cover/blind) is a reasonable
  next `DEVICE_TYPES` entry (see the comment above that array in
  `index.html`).
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
