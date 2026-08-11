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
- **Test Product (step 4)** — a real Web Serial monitor (Chrome/Edge
  only, with a warning banner elsewhere). Connect to a board already
  flashed via the Docker build (see `docs/getting-started.md`), pick a
  baud rate (115200 by default, matching `idf.py monitor`), and watch its
  live log — e.g. commissioning output. This is intentionally *not* tied
  to a wizard-generated firmware image (that doesn't exist yet); it just
  monitors whatever is already on the board. The "Download the generated
  firmware" card is a visible-but-disabled stub until Generate Firmware
  exists. Testing is optional — Next is always enabled on this step.
- **Customise & Review (step 5)** — a review table (Product name, Device
  type, Module, Driver + IO pin) with per-row **Edit** links that jump
  straight back to the relevant step, plus a **Generated configuration
  preview**: the actual `idf.py set-target <chip>` command and a unified
  diff for the one line this wizard can honestly promise to change —
  `LIGHT_LED_GPIO` in `app_main.cpp`. A **Copy** button puts both on the
  clipboard. Next is disabled if any earlier step is incomplete. Nothing
  is written to disk — you still apply the diff yourself through the
  Docker build.
- **Step 6** (`Generate Firmware`) is intentionally stubbed ("Coming
  soon") — the stepper and Back/Next navigation already work, selections
  made so far are shown, just no content yet.

## Next steps (not yet built)

1. **Generate Firmware** — replaces "Place Order" from the reference UI;
   this is open-source and local, so the end state is a generated
   config/command to run through the Docker build, not a purchase. Given
   Customise & Review already renders the diff/command text, this step
   could reasonably just be a "download as .patch / .sh" convenience
   rather than new logic.

## Design notes

- Single self-contained `index.html` — plain HTML/CSS/vanilla JS, no
  dependencies, matches the "no hidden code" principle in `CLAUDE.md`.
- State persists only in `localStorage`; clearing browser data resets it.
