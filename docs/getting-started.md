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

This image bundles ESP-IDF *and* esp-matter, already installed and exported:

```bash
docker pull espressif/esp-matter:latest
```

> The plain `espressif/idf` image used in the HomeKit guide does **not** contain
> esp-matter — that's why we use `espressif/esp-matter` here.

## 3. Clone the repository

```bash
git clone --recursive https://github.com/AchimPieters/esp32-matter.git
cd esp32-matter
```

## 4. Start the container

Use the helper:

```bash
./tools/dev.sh
```

…which is equivalent to the StudioPieters-style command:

```bash
docker run --rm -it -v "$PWD":/project -w /project espressif/esp-matter:latest /bin/bash
```

You're now inside the container, in `/project`, with the whole SDK available.

## 5. Build the light (inside the container)

```bash
. "$IDF_PATH/export.sh" && . "$ESP_MATTER_PATH/export.sh"
cd firmware/light
idf.py set-target esp32          # or esp32c3 / esp32c6 / esp32s3 / esp32h2
idf.py build
```

The build output lands in `firmware/light/build/`, which is visible on your host
too (thanks to the mounted volume).

## 6. Flash from the host

Docker Desktop on macOS/Windows can't reach the USB serial port, so flash from
your host (outside the container) with esptool:

```bash
esptool.py -p <PORT> write_flash \
  0x0     firmware/light/build/bootloader/bootloader.bin \
  0x8000  firmware/light/build/partition_table/partition-table.bin \
  0x20000 firmware/light/build/matter_light.bin
```

Find `<PORT>` with `ls /dev/tty.*` (macOS) or `ls /dev/ttyUSB*` (Linux). On Linux
you *can* also pass the device into Docker with `--device=/dev/ttyUSB0` and use
`idf.py flash` directly, but the host route above works everywhere.

## 7. Generate your own QR code (offline, inside the container)

```bash
pip install esp-matter-mfg-tool
../../tools/gen_factory.sh
```

This writes, under `out/<vid_pid>/<uuid>/`:

- `*-partition.bin` — the factory partition (certificates + commissioning data)
- `*-qrcode.png` — the QR code to scan
- a manual pairing code (also in the generated CSV)

Flash the factory partition to the `fctry` slot (from the host):

```bash
esptool.py -p <PORT> write_flash 0x3E0000 firmware/light/out/**/**-partition.bin
```

## 8. Commission the device

Open your controller app — Apple Home, Google Home, Amazon Alexa, SmartThings,
or Home Assistant — choose *Add device / Add Matter device*, and scan the QR
code. Toggle it and watch the LED (and the serial log) respond.

## 9. Next steps

- Duplicate `firmware/light/` for other device types.
- Push a `v*` tag to build and publish a Release automatically.
- Harden for real use: see `../SECURITY.md`.
