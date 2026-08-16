> **A note from esp32-matter (this repo) before Espressif's own README below:**
>
> This is Espressif's own reference Camera example from the esp-matter SDK
> (`$ESP_MATTER_PATH/examples/camera`), reproduced here verbatim
> (Public Domain/CC0, per its own file headers) rather than rewritten —
> rebuilding ~5,300 lines of production WebRTC/Matter integration code
> from scratch would be both infeasible in any reasonable time and
> strictly worse than reusing Espressif's own tested implementation. This
> is the same "port a real, working reference rather than guess"
> principle already used elsewhere in this repo (SM2335EGH, APA102,
> OpenTherm in `firmware/thermostat/`), just at a much larger scale.
>
> **This device type is architecturally unlike every other one in this
> repo — read this before assuming it works the same way:**
>
> - **Two chips, two firmware images, one physical board.** Matter's real
>   Camera device type (`CameraAVStreamManagement` + `WebRTCTransportProvider`
>   clusters, live WebRTC video) needs both Matter signaling *and* real
>   video capture/H.264 encoding at once — more than any single chip this
>   repo otherwise targets can do. Espressif's own answer is a split
>   architecture across the **ESP32-P4 Function EV Board**, which has an
>   ESP32-P4 (camera + hardware video encode) and an ESP32-C6 (Wi-Fi/BLE
>   + Matter) on one board, talking over SDIO. `firmware/camera/` (this
>   folder) is only the **ESP32-C6 signaling half** — build it for
>   `esp32c6` (or `esp32c5`), matching its own `sdkconfig.defaults.esp32c6`/
>   `.esp32c5`, not this repo's default `esp32` target. The **ESP32-P4
>   media half is not part of this repo at all** — it's the KVS SDK's own
>   `streaming_only` example, built and flashed straight from the
>   externally-cloned SDK below, per Espressif's own instructions further
>   down this file.
> - **A real external SDK dependency, unlike anything else here.** Every
>   other device type in this repo only needs the pinned
>   `espressif/esp-matter` Docker image — nothing else. This one also
>   needs the [Amazon Kinesis Video Streams WebRTC
>   SDK](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c)
>   (`beta-reference-esp-port` branch, with its own submodules —
>   `libwebsockets`, `libsrtp2`, `usrsctp`, the KVS PIC/producer-c
>   libraries) cloned separately and pointed to via `KVS_SDK_PATH`,
>   exactly as Espressif's own instructions below describe. This repo
>   does not vendor or bundle that SDK — per this project's own
>   "no hidden code" principle, you clone it yourself, the same way you
>   already clone this repo and pull the Docker image.
> - **Build-verified for real**, not assumed: built successfully in the
>   pinned `espressif/esp-matter:release-v1.6_idf_v5.5.4` Docker image
>   for `esp32c6`, with `KVS_SDK_PATH` pointing at a real, freshly cloned
>   + submodule-initialized copy of the SDK above — see the
>   repository-layout entry in this repo's own `CLAUDE.md` for the exact
>   result. **Not hardware-tested** — an ESP32-P4 Function EV Board was
>   not physically available when this was added; this is the first
>   device type in this repo where even the Matter-facing firmware alone
>   needs hardware several tiers more specialized/expensive than
>   everything else here.
> - **Not in the product wizard.** `tools/product-wizard/` assumes one
>   device type = one ESP32 chip = one firmware image on one board — a
>   two-chip, two-firmware, external-SDK device fundamentally doesn't fit
>   that model, so it isn't offered there. Build and flash this one by
>   hand, following Espressif's own instructions below.

# Matter Camera

This example creates a Camera device using the ESP Matter data model.

# Split Mode Camera Example

This example demonstrates a **two-chip split architecture** for ESP32
Camera, where signaling and media streaming are separated across two processors
for optimal power efficiency.

## Architecture Overview

The split mode consists of two separate firmware images:

### 1. **matter_camera** (ESP32-C6)

-   **Role**: Matter camera with WebRTC signaling integration
-   **Responsibilities**:
    -   WebRTC signaling
    -   Bridge communication with media adapter
    -   Always-on connectivity for instant responsiveness

### 2. **media_adapter** (ESP32-P4)

-   **Role**: Media streaming device
-   **Implementation**: Uses the `streaming_only` example from
    `${KVS_SDK_PATH}/esp_port/examples/streaming_only`
-   **Responsibilities**:
    -   Video/audio capture and encoding
    -   WebRTC media streaming
    -   Power-optimized operation (sleeps when not streaming)
    -   Receives signaling commands via bridge from esp32_camera

## Hardware Requirements

-   **ESP32-P4 Function EV Board** (required)
    -   Contains both ESP32-P4 and ESP32-C6 processors
    -   Built-in camera support
    -   SDIO communication between processors

## System Architecture

```
┌─────────────────┐      SDIO Bridge     ┌─────────────────┐
│    ESP32-C6     │◄────────────────────►│    ESP32-P4     │
│ (matter_camera) │      Communication   │ (media_adapter) │
│                 │                      │                 │
│ ┌─────────────┐ │                      │ ┌─────────────┐ │
│ │             │ │                      │ │ H.264       │ │
│ │   Matter    │ │                      │ │ Encoder     │ │
│ │             │ │                      │ │             │ │
│ │  Signaling  │ │                      │ │ Camera      │ │
│ │             │ │                      │ │ Interface   │ │
│ └─────────────┘ │                      │ └─────────────┘ │
└─────────────────┘                      └─────────────────┘
        ▲                                        ▲
        │                                        │
        ▼                                        ▼
   (Signaling)                              Video/Audio
                                             Hardware
```

## Quick Start

### Prerequisites

-   IDF version: v5.5.4
-   [ESP32-P4 Function EV Board](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html)
-   [Amazon Kinesis Video Streams WebRTC SDK repository](https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/tree/beta-reference-esp-port)

```
git clone https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c.git
git checkout beta-reference-esp-port
git submodule update --init --depth 1
export KVS_SDK_PATH=/path/to/amazon-kinesis-video-streams-webrtc-sdk-c
```
### Build and Flash Instructions
**Note**: This requires **TWO separate firmware flashes** on the same
ESP32-P4 Function EV Board.
#### Step 1: Flash camera example (ESP32-C6)
This handles WebRTC signaling and Matter integration.
```bash
idf.py set-target esp32c6
idf.py build
idf.py -p [PORT] flash monitor
```

*__NOTE__*:
- ESP32-C6 does not have an onboard UART port. You will need to use [ESP-Prog](https://docs.espressif.com/projects/esp-iot-solution/en/latest/hw-reference/ESP-Prog_guide.html) board or any other JTAG.
- Use following Pin Connections:

| ESP32-C6 (J2/Prog-C6) | ESP-Prog |
|----------|----------|
| IO0      | IO9      |
| TX0      | TXD0     |
| RX0      | RXD0     |
| EN       | EN       |
| GND      | GND      |

#### Step 2: Flash media_adapter (ESP32-P4)

This handles video/audio streaming. The firmware is the `streaming_only` example
from the KVS SDK.

```bash
cd ${KVS_SDK_PATH}/esp_port/examples/streaming_only
idf.py set-target esp32p4
idf.py menuconfig
# Go to Component config -> ESP System Settings -> Channel for console output
# (X) USB Serial/JTAG Controller # For ESP32-P4 Function_EV_Board V1.2 OR V1.5
# (X) Default: UART0 # For ESP32-P4 Function_EV_Board V1.4
idf.py build
idf.py -p [PORT] flash monitor
```

**Note**: If the console selection is wrong, you will only see the initial
bootloader logs. Please change the console as instructed above and reflash the
app to see the complete logs.

**Note**: Currently, due to flash size limitations of ESP32-C6 onboard the
ESP32-P4 Function EV Board, the `ota_1` partition (see
[`partitions.csv`](partitions.csv)) is disabled and the size of the `ota_0`
partition is increased. This prevents the firmware from performing OTA updates.
Hence, this configuration is not recommended for production use.
