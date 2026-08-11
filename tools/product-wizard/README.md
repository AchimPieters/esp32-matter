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
  `localStorage`, nothing is sent anywhere).
- **Create a new product → Setup your product** — name your product and
  hit **Start**.
- **Get Started (step 1)** — pick a device type. Only "On/Off Light" exists
  today (matches `firmware/light/`), so it's auto-selected; the card list
  is ready for more types once they're added.
- **Select Module (step 2)** — pick a target chip (ESP32 / C3 / C6 / S3 /
  H2), mirroring what `tools/dev.sh` + `idf.py set-target` actually
  support. Connectivity badges (Wi-Fi/BLE/Thread) reflect each chip's real
  radios — e.g. ESP32-H2 has no Wi-Fi.
- **Configure Device (step 3)** — set the LED GPIO pin for the on/off
  light's digital-GPIO driver (the only driver `firmware/light/` actually
  has — no PWM/dimming yet, unlike the ESP ZeroCode screenshots this is
  modelled on). Defaults per module echo the comment in
  `app_main.cpp`. A "Configuration summary" sidebar mirrors the reference
  UI. Purely a value capture for now — it does **not** edit
  `app_main.cpp`'s `LIGHT_LED_GPIO` yet; that's Generate Firmware's job.
- **Steps 4–6** (`Test Product`, `Customise & Review`, `Generate
  Firmware`) are intentionally stubbed ("Coming soon") — the stepper and
  Back/Next navigation already work, selections made so far are shown,
  just no content yet.

## Next steps (not yet built)

1. **Test Product** — likely out of scope for a static page (needs
   WebSerial/USB access); may link out to the flashing instructions in
   `docs/getting-started.md` instead.
2. **Customise & Review** — summary screen before generating.
3. **Generate Firmware** — replaces "Place Order" from the reference UI;
   this is open-source and local, so the end state is a generated
   config/command (and ideally an actual patch to
   `firmware/light/main/app_main.cpp`'s `LIGHT_LED_GPIO`) to run through
   the Docker build, not a purchase.

## Design notes

- Single self-contained `index.html` — plain HTML/CSS/vanilla JS, no
  dependencies, matches the "no hidden code" principle in `CLAUDE.md`.
- State persists only in `localStorage`; clearing browser data resets it.
