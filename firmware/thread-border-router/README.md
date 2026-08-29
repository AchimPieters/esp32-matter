> **A note from esp32-matter (this repo) before Espressif's own README below:**
>
> This is Espressif's own reference Thread Border Router example from the
> esp-matter SDK (`$ESP_MATTER_PATH/examples/thread_border_router`),
> reproduced here (Public Domain/CC0, per its own file headers) rather
> than rewritten — the same "port a real, working reference rather than
> guess the integration shape" principle firmware/camera/'s,
> firmware/ble-mesh-bridge/'s, and firmware/zigbee-bridge/'s own
> repository-layout entries already establish. Only one line was changed
> from Espressif's own original: `CMakeLists.txt`'s own `EXTRA_COMPONENT_DIRS`
> used a path (`"../common"`) relative to the *original* example's own
> location inside the SDK tree (`examples/thread_border_router/`) — moved
> here to `firmware/thread-border-router/`, a different depth, so it no
> longer resolved to `examples/common`. Fixed to the absolute
> `"${ESP_MATTER_PATH}/examples/common"` instead — the exact same fix
> firmware/zigbee-bridge/'s own copy of this identical pattern already
> needed and got.
>
> **Confirmed against the CSA's own `data_model/1.6/device_types/
> ThreadBorderRouter.xml` (device type 0x0091, revision 2) before porting
> this**, not assumed from the folder name alone: `class="simple"
> scope="endpoint"` — genuinely a standalone-buildable endpoint device
> type, NOT `class="utility"` (the same wrong "must always be composed"
> assumption this repo's own CLAUDE.md already documents correcting once,
> for Temperature Controlled Cabinet — re-checked properly here from the
> start). Thread Network Diagnostics (0x0035) and Thread Border Router
> Management (0x0452) are both `<mandatoryConform/>`; Thread Network
> Directory (0x0453) is `<optionalConform/>` and not implemented by this
> reference. `endpoint::thread_border_router::create()` confirmed
> complete — its own `add()` (read directly in esp-matter's legacy
> `esp_matter_endpoint.cpp`) creates BOTH mandatory clusters itself, no
> manual cluster addition needed. `cluster::thread_border_router_management
> ::create()` is genuinely Delegate-based (`config->delegate`, wired via
> `ThreadBorderRouterManagementDelegateInitCB`) — this example supplies a
> REAL delegate, `GenericOpenThreadBorderRouterDelegate` (connectedhomeip's
> own, in `platform/OpenThread/GenericThreadBorderRouterDelegate.h`),
> wired directly to the actual OpenThread Border Router stack via
> `esp_openthread_border_router_init()` — genuine, working Thread
> networking functionality (dataset get/set commands, PAN Change feature),
> not a stub or a mock (a real `mock_thread_border_router_management_
> delegate.h/.cpp` also exists in esp-matter's own `examples/
> all_device_types_app`, for a non-functional structural-only case — NOT
> what this file uses).
>
> **This device type is architecturally unlike every other one in this
> repo except firmware/camera/ and firmware/zigbee-bridge/ — read this
> before assuming it works the same way:**
>
> - **Genuinely different in kind from this repo's own bridges
>   (firmware/ble-mesh-bridge/, firmware/rf-ir-bridge/, firmware/
>   zigbee-bridge/).** Those all expose OTHER, non-Matter devices as
>   dynamic Matter endpoints on ONE fabric. This device is Thread's own
>   network-infrastructure role: it runs the actual OpenThread Border
>   Router daemon (RA/RIO route advertisement, NAT64/DNS64 where
>   applicable, an infrastructure-interface bridge between a Thread mesh
>   and your LAN/Wi-Fi) so that SEPARATE Thread end-devices (their own
>   independent Matter nodes, each on the same fabric once individually
>   commissioned) can actually reach your network and your controller at
>   all. This repo's own earlier "controller/media/infrastructure device
>   types are always out of scope" framing had correctly ruled Thread
>   Border Router out on exactly this basis — revisited and built anyway
>   on explicit request, with this real architectural distinction kept
>   honest in this note rather than blurred with the bridge family above.
> - **Two chips, two firmware images, one physical setup** — the same
>   shape firmware/camera/'s and firmware/zigbee-bridge/'s own
>   repository-layout entries already establish, for the same underlying
>   reason firmware/zigbee-bridge/'s own entry documents: a Thread/
>   802.15.4 radio running its own dedicated stack, at the same time
>   Matter itself needs Wi-Fi/BLE, is more than one chip can practically
>   do at once. Espressif's own answer, and the one this example is
>   built against: the real **ESP Thread Border Router board** (an
>   integrated module combining an ESP32-S3 and an ESP32-H2 — see
>   [espressif/esp-thread-br](https://github.com/espressif/esp-thread-br)),
>   with the ESP32-H2 running as a plain OpenThread RCP (Radio
>   Co-Processor, from ESP-IDF's own `examples/openthread/ot_rcp` — the
>   exact same RCP role, and the exact same underlying `ot_rcp` reference,
>   firmware/zigbee-bridge/'s own ESP32-H2 half already uses, just for
>   Thread instead of Zigbee) and the ESP32-S3 (`firmware/
>   thread-border-router/`, this folder) running this actual Matter +
>   OpenThread-Border-Router application, talking to the H2 over UART.
>   Optionally, with `CONFIG_AUTO_UPDATE_RCP=y` (set in this file's own
>   shipped `sdkconfig.defaults`, unmodified from Espressif's own
>   default), the S3 can flash the H2's RCP firmware for you automatically
>   from a bundled SPIFFS image — a real, working feature of this
>   reference, not something added here.
> - **Build for `esp32s3`, not this repo's default `esp32` target** — the
>   ESP32-H2 half is not part of this repo at all; build and flash it by
>   hand from ESP-IDF's own `examples/openthread/ot_rcp` (`idf.py
>   set-target esp32h2`), same as firmware/zigbee-bridge/'s own RCP half.
> - **Build-verified for real**, not assumed: built successfully in the
>   pinned `espressif/esp-matter:release-v1.6_idf_v5.5.4` Docker image for
>   `esp32s3` (Espressif's own only tested/documented target for this
>   example). **Not hardware-tested** — no real ESP Thread Border Router
>   board (or a standalone ESP32-S3 + ESP32-H2 pair wired the same way)
>   was physically available when this was added, and verifying this one
>   for real also needs a genuine Thread end-device to actually
>   commission onto the resulting Thread network.
> - **Not in the product wizard** — same reasoning as firmware/camera/,
>   firmware/ble-mesh-bridge/, and firmware/zigbee-bridge/: a two-chip,
>   two-firmware device with no GPIO fields of its own (this board's own
>   radios/antennas are fixed, not something to wire up yourself) doesn't
>   fit `tools/product-wizard/`'s own one-device-type/one-chip/one-firmware
>   model.

# Thread Border Router

This example creates a Matter Thread Border Router device using the ESP Matter data model.


See the [docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html) for more information about building and flashing the firmware.

## 1. Additional Environment Setup

### 1.1 Hardware Platform

The [ESP Thread Border Router board](https://github.com/espressif/esp-thread-br?tab=readme-ov-file#esp-thread-border-router-board) which provides an integrated module of an ESP32-S3 and an ESP32-H2 is required for this example.

### 1.2 Firmware for RCP

The [OpenThread RCP](https://github.com/espressif/esp-idf/tree/master/examples/openthread/ot_rcp) should be run on ESP32-H2 of the Border Router board. You can flash it directly:


```
$ cd /path/to/esp-idf/examples/openthread/ot_rcp
$ idf.py set-target esp32h2 build
$ idf.py -p <port> erase-flash flash
```

Or you can flash the firmware of ESP32-H2 with [esp_rcp_update](https://github.com/espressif/esp-thread-br/tree/main/components/esp_rcp_update) after enabling `AUTO_UPDATE_RCP` in menuconfig:

```
$ cd /path/to/esp-idf/examples/openthread/ot_rcp
$ idf.py set-target esp32h2 build
```

After flashing the Thread Border Router firmware to ESP32-S3, it will flash the RCP firmware to ESP32-H2 automatically.

### 1.3 Firmware for Host SoC

The default setting flash size is 8MB, set target and build as below:

```
$ idf.py set-target esp32s3
$ idf.py build
```

## 2. Post Commissioning Setup

After commissioning the Border Router with chip-tool, you can set up the Thread network with ThreadBorderRouterManagement cluster.

```
$ ./chip-tool generalcommissioning arm-fail-safe 180 1 0x7283 0
$ ./chip-tool threadborderroutermanagement set-active-dataset-request hex:<thread-dataset-tlvs> 0x7283 1
$ ./chip-tool generalcommissioning commissioning-complete 0x7283 0
```
Then the Thread Border Router will form/join a Thread network and you can commission a Thread End-device to the Thread network with chip-tool.

```
$ ./chip-tool pairing ble-thread 0x7384 hex:<thread-dataset-tlvs> 20202021 3840
```
