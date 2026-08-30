# Getting started

A first-time walkthrough using the Docker-based ESP-IDF environment from the
StudioPieters guide (https://www.studiopieters.nl/esp32-homekit-development/),
adapted for Matter. From an empty machine to a paired Matter light.

The only thing installed on your host is Docker (for building) and esptool (for
flashing). Everything else lives inside the container.

## 1. Prerequisites

- An ESP32 board — a classic **ESP32 DevKit (WROOM-32)** matches this guide.
- A USB cable and a free USB port. Use a USB-to-serial adapter if your board
  has no on-board USB.
- **Docker Desktop** installed and running.
- Python 3 + **esptool** on the host for flashing:
  ```bash
  python3 -m ensurepip --upgrade
  pip3 install esptool
  esptool.py --version
  ```

## 2. Pull the esp-matter Docker image

This image bundles ESP-IDF *and* esp-matter, already installed and exported.
Pinned to esp-matter's own recommended ESP-IDF version (v5.5.4) for
reproducible builds — esp-matter doesn't support ESP-IDF v6.0.x yet:

```bash
docker pull espressif/esp-matter:release-v1.6_idf_v5.5.4
```

> The plain `espressif/idf` image used in the HomeKit guide does **not** contain
> esp-matter — that's why we use `espressif/esp-matter` here.

## 3. Clone the repository

```bash
git clone https://github.com/AchimPieters/esp32-matter.git
cd esp32-matter
```

(No `--recursive` needed — esp-matter and connectedhomeip live inside the
Docker image, not as git submodules of this repo.)

## 4. Start the container

Use the helper:

```bash
./tools/dev.sh
```

…which is equivalent to the StudioPieters-style command:

```bash
docker run --rm -it -v "$PWD":/project -w /project espressif/esp-matter:release-v1.6_idf_v5.5.4 /bin/bash
```

You're now inside the container with the whole SDK already activated — the
image's entrypoint runs both `export.sh` scripts for you before handing you
the shell. That entrypoint also leaves you in `$ESP_MATTER_PATH`
(`/opt/espressif/esp-matter`), **not** `/project`, regardless of the `-w`
flag passed to `docker run`. Your repo is still mounted at `/project`; just
use the absolute path to get there.

## 5. Build the light (inside the container)

```bash
cd /project/firmware/light
idf.py set-target esp32          # or esp32c2 / esp32c3 / esp32c5 / esp32c6 / esp32c61 / esp32s3 / esp32h2
idf.py build
```

The build output lands in `firmware/light/build/`, which is visible on your host
too (thanks to the mounted volume).

## 6. Generate your own QR code (offline, inside the container)

Still inside the container, from the firmware folder:

```bash
pip install esp-matter-mfg-tool          # once; chip-cert already ships in the image
cd /project/firmware/light
/project/tools/gen_factory.sh
```

This writes, under `firmware/light/out/<vid_pid>/<uuid>/`:

- `*-partition.bin` — the factory partition (per-device certificates +
  commissioning data)
- `*-qrcode.png` — the QR code to scan
- a manual pairing code (also in the generated CSV)

Run this **once per physical unit** — every device needs its own identity.

## 7. Flash from the host

Docker Desktop on macOS/Windows can't reach the USB serial port, so flash from
your host (outside the container) with esptool. Find `<PORT>` with
`ls /dev/tty.*` (macOS) or `ls /dev/ttyUSB*` (Linux).

```bash
esptool.py --chip esp32 -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000   firmware/light/build/bootloader/bootloader.bin \
  0x8000   firmware/light/build/partition_table/partition-table.bin \
  0x10000  firmware/light/build/ota_data_initial.bin \
  0x20000  firmware/light/build/matter_light.bin \
  0x3E0000 firmware/light/out/*/*/*-partition.bin
```

Notes:

- **Bootloader offset is `0x1000` on the classic ESP32**, but `0x0` on every
  later chip (C2/C3/C5/C6/C61/S3/H2). `idf.py build` prints the authoritative
  offsets for whatever you just built.
- **`ota_data_initial.bin` at `0x10000` is required** — this partition table has
  no "factory" app slot, so the bootloader uses the OTA data partition to pick a
  boot slot.
- **`0x3E0000` is the `fctry` partition** — the factory data / QR identity from
  step 6.

On Linux you can alternatively pass the device into Docker with
`--device=/dev/ttyUSB0` and use `idf.py flash` directly.

## 8. Commission the device

Open your controller app — **Home Assistant** is recommended for this repo's
test certificates (see the note below) — choose *Add device / Add Matter
device*, and scan the QR code. Toggle it and watch the LED (and the serial log)
respond.

## 9. Next steps

- Try the **Product Wizard** (`tools/product-wizard/index.html`) — it generates
  the build + flash commands for any of the 69 device types.
- Browse the [device catalog](../README.md#the-device-catalog-69-types) and
  duplicate the closest firmware folder for your own device.
- Harden for real use: see `../SECURITY.md`.
- Read `../CLAUDE.md` for the full per-device engineering log.

> Note: Apple Home / Google Home reject this repo's self-signed **test**
> certificates in their normal consumer flow. Home Assistant and `chip-tool`
> commission them fine, and stay fully local.
