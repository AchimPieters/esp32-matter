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
- **Wizard shell** — the step indicator (`Get Started → Select Module →
  Configure Device → Test Product → Customise & Review → Generate
  Firmware`) renders, but only the first step is wired up. The rest are
  intentionally stubbed ("Coming soon") — this is scaffolding to build on,
  not a finished wizard.

## Next steps (not yet built)

1. **Select Module** — pick a target chip (ESP32 / C3 / C6 / S3 / H2),
   mirroring `tools/dev.sh`'s supported targets.
2. **Configure Device** — pick a device type from `firmware/` (currently
   just the on/off light) and its GPIO/driver options, writing out a
   `sdkconfig.defaults` diff or similar.
3. **Test Product** — likely out of scope for a static page (needs
   WebSerial/USB access); may link out to the flashing instructions in
   `docs/getting-started.md` instead.
4. **Customise & Review** — summary screen before generating.
5. **Generate Firmware** — replaces "Place Order" from the reference UI;
   this is open-source and local, so the end state is a generated
   config/command to run through the Docker build, not a purchase.

## Design notes

- Single self-contained `index.html` — plain HTML/CSS/vanilla JS, no
  dependencies, matches the "no hidden code" principle in `CLAUDE.md`.
- State persists only in `localStorage`; clearing browser data resets it.
