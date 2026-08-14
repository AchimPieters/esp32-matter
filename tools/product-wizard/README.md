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
  Sensor" (`firmware/contact-sensor/`), "Outlet" (`firmware/outlet/`), or
  "Temperature Sensor" (`firmware/temperature-sensor/`). All five are
  real, buildable firmware, not just UI placeholders.
- **Select Module (step 2)** — pick a target chip (ESP32 / C3 / C6 / S3 /
  H2), mirroring what `tools/dev.sh` + `idf.py set-target` actually
  support. Connectivity badges (Wi-Fi/BLE/Thread) reflect each chip's real
  radios — e.g. ESP32-H2 has no Wi-Fi.
- **Configure Device (step 3)** — set the GPIO(s) each device type's
  driver actually exposes: the LED pin for the light, the button pin for
  the switch, the contact pin for the contact sensor, the output + button
  pins for the outlet, the I2C SDA + SCL pins for the temperature sensor
  (the two device types needing a second GPIO — see `DEVICE_TYPES`'
  optional `secondary` field in `index.html`, alongside `driver` and
  `identify`; label-driven so it reads as "Button" for the outlet and
  "SCL" for the temperature sensor, not hardcoded either way). No
  PWM/dimming or debounce config yet, unlike the ESP ZeroCode screenshots
  this is modelled on. Defaults per module
  echo the comments in each `app_main.cpp`. Also an **Identify LED**
  checkbox, on by default — every device type has one, since it's a real
  Matter cluster (blinks in response to a controller's "Identify"
  command) implemented in both firmware files, not just a wizard-only
  option. Untick it and the wizard leaves that `#define` alone, so the
  firmware's shipped default GPIO stays in effect — the LED still exists
  in the compiled firmware either way, this only controls whether the
  wizard customises its pin. A "Configuration summary" sidebar mirrors
  the reference UI. Purely a value capture for now — it does **not** edit
  the firmware file yet; that's Generate Firmware's job.
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
Review → Generate Firmware. All five device types on classic ESP32 have
now been validated for real, through the wizard's own generated commands
run verbatim — built, factory data + QR generated, flashed, and
commissioned via Apple Home (full PASE/CASE handshake, no errors).

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
`secondary` GPIO field: a `sensorModels` list per device type plus a
third sed target (`#define SENSOR_TYPE ...`) alongside the two GPIO
fields, since which driver compiles in at all is now a real choice, not
just a pin number.

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

- Five device types exist (`On/Off Light`, `On/Off Switch`, `Contact
  Sensor`, `Outlet`, `Temperature Sensor`) — light/switch/contact/outlet
  are all digital GPIO, temperature is this repo's first non-GPIO sensor
  (I2C, single-wire, or 1-Wire depending which of its 7 supported chips
  you pick). Adding a device using analog/ADC input (untouched by any
  existing type) is the natural next `DEVICE_TYPES` entry (see the
  comment above that array in `index.html`).
- The switch's button sends a real OnOff Toggle to whatever it's bound to
  (`client::cluster_update()`), but that binding itself has to be set up
  through a controller with a Bindings UI (e.g. Home Assistant) — the
  wizard doesn't help with that part.
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
