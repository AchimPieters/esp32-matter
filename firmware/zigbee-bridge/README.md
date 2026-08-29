> **A note from esp32-matter (this repo) before Espressif's own README below:**
>
> This is Espressif's own reference Zigbee Bridge example from the
> esp-matter SDK (`$ESP_MATTER_PATH/examples/bridge_apps/zigbee_bridge`),
> reproduced here verbatim (Public Domain/CC0, per its own file headers)
> rather than rewritten — the same "port a real, working reference rather
> than guess the integration shape" principle firmware/camera/'s own and
> firmware/ble-mesh-bridge/'s own repository-layout entries already
> establish, and the same real Matter **bridge** (Aggregator) architecture
> firmware/ble-mesh-bridge/'s own preamble documents in full (dynamic
> Matter endpoints created/removed at runtime, backed by the shared
> `esp_matter_bridge`/`examples/common/app_bridge` infrastructure every
> bridge in this repo builds on).
>
> **This device type is architecturally unlike every other one in this
> repo except firmware/camera/ — read this before assuming it works the
> same way:**
>
> - **Two chips, two firmware images, one physical setup** — the exact
>   same shape firmware/camera/'s own repository-layout entry already
>   establishes, for a genuinely different reason here: Zigbee needs its
>   own dedicated 802.15.4 radio running its own coordinator stack at the
>   same time Matter itself needs Wi-Fi/BLE — more than one chip can
>   practically do at once. Espressif's own answer: an **ESP32-S3**
>   (running this actual Matter/bridge application — `firmware/
>   zigbee-bridge/`, this folder) talking over UART to an **ESP32-H2**
>   running as a Zigbee **RCP** (Radio Co-Processor), built from ESP-IDF's
>   own `examples/openthread/ot_rcp` reference (repurposed for Zigbee via
>   its own `OPENTHREAD_NCP_VENDOR_HOOK` option) — genuinely plain
>   ESP-IDF, not esp-matter, and not part of this repo at all; build and
>   flash it by hand following Espressif's own instructions further down
>   this file. A "Zigbee Gateway DevKit" board combining both chips on
>   one PCB also exists per Espressif's own documentation below; standalone
>   DevKitC boards wired together over UART work identically.
> - **Build for `esp32s3`, not this repo's default `esp32` target** — this
>   folder's own `CMakeLists.txt` device-path selection also supports
>   plain `esp32`/`esp32c3`, but Espressif's own tested/documented
>   configuration (and the one build-verified here) is `esp32s3`.
> - **Build-verified for real**, not assumed: built successfully in the
>   pinned `espressif/esp-matter:release-v1.6_idf_v5.5.4` Docker image for
>   `esp32s3` — clean on the first attempt, though flash headroom is
>   genuinely tight: only 4% of the smallest app partition free (`idf.py
>   build` itself warns "The smallest app partition is nearly full"), the
>   least headroom of any device type in this repo, worth knowing before
>   adding OTA-update payloads or other growth on top of this firmware as
>   shipped. **Not hardware-tested** — no ESP32-S3/ESP32-H2 pair (or
>   Zigbee Gateway DevKit board) was physically available when this was
>   added, and — like firmware/ble-mesh-bridge/ — verifying this one for
>   real also needs a genuine Zigbee peripheral device on the network
>   being bridged.
> - **Not in the product wizard** — same reasoning as firmware/camera/
>   and firmware/ble-mesh-bridge/: a two-chip, two-firmware device with no
>   GPIO fields of its own (every bridged device's own hardware lives on
>   a completely separate Zigbee peripheral) doesn't fit `tools/
>   product-wizard/`'s own one-device-type/one-chip/one-firmware model.

# Zigbee Bridge

This example demonstrates a Matter-Zigbee Bridge that bridges Zigbee devices to Matter fabric.

The Matter Bridge device is composed of two parts: The RCP running on ESP32-H2 and the bridge app running on ESP32-S3.

See the [docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html) for more information about building and flashing the firmware.

💡 Important:  `create_bridge_devices` callback can be used to add data model elements (e.g., attributes, commands, etc.) to the bridge endpoint.

## 1. Additional Environment Setup

### 1.1 Hardware connection

There are two hardware type options for this example. You can choose one of the two options in menuconfig `ESP Matter Zigbee Bridge Example`->`Zigbee Bridge board type`.

#### 1.1.1 Standalone DevKit boards
Connect the two SoCs via UART, below is an example setup with ESP32-S3 DevKitC and ESP32-H2 DevKitC:

![Zigbee Bridge Hardware Connection](../../docs/_static/zigbee_bridge_hardware_connection.jpg)

|  ESP32-S3 Pin  | ESP32-H2 Pin |
|----------------|--------------|
|   GND          |    GND       |
|   GPIO4        |    TX        |
|   GPIO5        |    RX        |

#### 1.1.2 Zigbee Gateway DevKit board

![Zigbee Gateway DevKit Board](../../docs/_static/esp-thread-border-router-zigbee-gateway-board.png)

### 1.2 Build and flash the RCP (ESP32-H2)

Please use the [ot_rcp](https://github.com/espressif/esp-idf/tree/master/examples/openthread/ot_rcp) example to build the RCP for the ZigBee Bridge.

```
cd $IDF_PATH/examples/openthread/ot_rcp/
idf.py menuconfig -> OPENTHREAD_NCP_VENDOR_HOOK=y
idf.py set-target esp32h2
idf.py -p <port> build flash
```

**Note**: The two SoCs on the Zigbee Gateway DevKit board use USB ports while the standalone DevKit boards use UART ports.

### 1.3 Build and flash the Bridge (ESP32-S3)

For Standalone DevKit boards:

```
cd ${ESP_MATTER_PATH}/examples/zigbee_bridge
idf.py set-target esp32s3
idf.py -p <port> build flash
```

For Zigbee Gateway board:

```
cd ${ESP_MATTER_PATH}/examples/zigbee_bridge
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.zb_gw_board" set-target esp32s3 build
idf.py -p <port> flash
```

The Matter Zigbee Bridge will run on the ESP32-S3 and Zigbee network will be formed.

## 2. Post Commissioning Setup

### 2.1 Discovering Zigbee Devices

Commissioning the Matter Zigbee Bridge first, 0x7283 in description below is its Node Id.

You can read the PartsList from the Bridge to get the number of the bridged devices.

```
descriptor read parts-list 0x7283 0x0
```

If there is no other Zigbee device on the Zigbee Network, you will get a result with only an aggregator endpoint. Example:

```
Data = [
    1,  <---------------------------- Aggregator Endpoint
],
```

Then read the PartsList from the Aggregator Endpoint, you will get an empty result.

```
descriptor read parts-list 0x7283 0x1
...
Data = [

],
...
```

### 2.2 Setup Zigbee Bulb on ESP32-H2

Build and run Zigbee Bulb app on another ESP32-H2 board.

```
cd /path/to/esp-zigbee-sdk/examples/esp_zigbee_HA_sample/HA_on_off_light
idf.py set-target esp32h2
idf.py -p <port> build flash monitor
```

The Zigbee Bulb will be added to the Zigbee Network and a dynamic endpoint will be added on the Bridge device. You can read the PartsList on Aggregator Endpoint again to get the dynamic endpoint ID.

```
descriptor read parts-list 0x7283 0x1
```

The data will now contain the information of the connected Zigbee devices. Example:

```
Data = [
    2,  <--------------------------Bridged Endpoint
],
```

It means that the Zigbee Bulb is added as Endpoint 1 on the Zigbee Bridge. You can read the DeviceTypeList on the dynamic endpoint.

```
descriptor read device-type-list 0x7283 2
```

You will get the device types of the endpoint:

```
DeviceTypeList: 2 entries
  [1]: {
    Type: 19 <-------------------- Bridged Node device type
    Revision: 1
  }
  [2]: {
    Type: 256 <-------------------- OnOff Light device type
    Revision: 2
  }
```

You can also read the cluster ServerList on the dynamic endpoint.

```
descriptor read server-list 0x7283 0x1
```

This will give the list of supported server clusters. Example:

```
OnDescriptorServerListListAttributeResponse: 4 entries
    [1]: 3
    [2]: 4
    [3]: 5
    [4]: 6    <------------------------ OnOff Cluster
    [5]: 57   <------------------------ Bridged Device Basic Information Cluster
```

### 2.3 Control the bulb with chip-tool

Now you can control the Zigbee bulb using the chip tool.

```
onoff toggle 0x7283 0x2
```

## 3. Device Performance

### 3.1 Memory usage

The following is the Memory and Flash Usage.

-   `Bootup` == Device just finished booting up. Device is not
    commissioned or connected to wifi yet.
-   `After Commissioning` == Device is connected to wifi and is also
    commissioned and is rebooted.
-   `After Adding a Bridged device` == A Zigbee OnOff Light is added
    on the Bridge.
-   device used: esp32_devkit_c
-   tested on:
    [b40bf8e3](https://github.com/espressif/esp-matter/commit/b40bf8e398161bcac515fd57eb13d14e031e3a91)
    (2023-04-17)
-   IDF: release/v5.1 [420ebd20](https://github.com/espressif/esp-idf/commit/420ebd208ae9eb71cb71ebd22742d1175f11addc)

|                         | Bootup | After Commissioning | After Adding a Bridged device |
|:-                       |:-:     |:-:                  |:-:                            |
|**Free Internal Memory** |40KB    |113KB                |109KB                          |

**Flash Usage**: Firmware binary size: 1.6MB

This should give you a good idea about the amount of free memory that is
available for you to run your application's code.

Applications that do not require BLE post commissioning, So BLE is disable
once commissioning is complete in the test.
