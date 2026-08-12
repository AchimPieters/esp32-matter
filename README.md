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
setup; the code also runs on ESP32-C3/C6/S3/H2.

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
├── .github/workflows/
│   └── build.yml           # CI: builds in the esp-matter image, attaches .bin to Releases
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
   idf.py set-target esp32        # or esp32c3 / esp32c6 / esp32s3 / esp32h2
   idf.py build
   ```

6. **Flash from your host** (Docker Desktop on macOS can't see the USB port, so
   flash outside the container — install esptool with `pip3 install esptool`):
   ```bash
   esptool.py -p <PORT> write_flash 0x0 firmware/light/build/bootloader/bootloader.bin \
       0x8000 firmware/light/build/partition_table/partition-table.bin \
       0x20000 firmware/light/build/matter_light.bin
   ```

7. **Generate your QR code (offline)** and **commission** — see
   `docs/getting-started.md`.

## Adding more device types

`firmware/switch/` is a second example, copied from `firmware/light/` with the
endpoint type swapped to `on_off_switch` — its button toggles its own OnOff
state, though sending a command to a bound device is still a TODO in its
`app_main.cpp`. To add another type, copy either folder and swap the endpoint
type in `app_main.cpp` — esp-matter provides ready-made types like
`dimmable_light`, `temperature_sensor`, `contact_sensor`, and many more.

## Updates via GitHub Releases

Push a tag and CI builds the firmware in the esp-matter image and publishes a
Release with the `.bin` attached:

```bash
git tag v1.0.0
git push origin v1.0.0
```

Flash a Release `.bin` over USB, or build Matter OTA on top so devices update
themselves over the air (start with USB; add OTA later).

## Honest expectations

This is a genuine firmware project, not a no-code app. You work from the command
line with ESP-IDF and C++ (inside Docker, so no local toolchain mess). That's the
price of "no hidden code, nothing shared" — in return you own and can read every
part of it.

## License

MIT — see `LICENSE`.
